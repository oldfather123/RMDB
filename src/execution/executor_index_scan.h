/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    void set_key_part(char *key, int offset, const ColMeta &col, const char *src) {
        memcpy(key + offset, src, col.len);
    }

    void set_key_min(char *key, int offset, const ColMeta &col) {
        memset(key + offset, 0, col.len);
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(key + offset, &v, sizeof(int));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::min();
            memcpy(key + offset, &v, sizeof(int64_t));
        } else if (col.type == TYPE_FLOAT) {
            float v = -std::numeric_limits<float>::infinity();
            memcpy(key + offset, &v, sizeof(float));
        }
    }

    void set_key_max(char *key, int offset, const ColMeta &col) {
        memset(key + offset, 0xFF, col.len);
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(key + offset, &v, sizeof(int));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::max();
            memcpy(key + offset, &v, sizeof(int64_t));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::infinity();
            memcpy(key + offset, &v, sizeof(float));
        }
    }

    const Condition *find_eq_cond(const ColMeta &col) const {
        for (auto &cond : fed_conds_) {
            if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ && cond.lhs_col.col_name == col.name &&
                cond.op == OP_EQ) {
                return &cond;
            }
        }
        return nullptr;
    }

    const Condition *find_lower_cond(const ColMeta &col, bool &inclusive) {
        const Condition *best = nullptr;
        for (auto &cond : fed_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name ||
                (cond.op != OP_GT && cond.op != OP_GE)) {
                continue;
            }
            if (best == nullptr ||
                compare_raw(cond.rhs_val.raw->data, best->rhs_val.raw->data, col.type, col.len) > 0) {
                best = &cond;
                inclusive = cond.op == OP_GE;
            }
        }
        return best;
    }

    const Condition *find_upper_cond(const ColMeta &col, bool &inclusive) {
        const Condition *best = nullptr;
        for (auto &cond : fed_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name ||
                (cond.op != OP_LT && cond.op != OP_LE)) {
                continue;
            }
            if (best == nullptr ||
                compare_raw(cond.rhs_val.raw->data, best->rhs_val.raw->data, col.type, col.len) < 0) {
                best = &cond;
                inclusive = cond.op == OP_LE;
            }
        }
        return best;
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        std::vector<char> lower_key(index_meta_.col_tot_len);
        std::vector<char> upper_key(index_meta_.col_tot_len);
        int offset = 0;
        bool has_lower = false;
        bool has_upper = false;
        bool lower_inclusive = true;
        bool upper_inclusive = true;
        bool stop_prefix = false;

        for (auto &col : index_meta_.cols) {
            if (stop_prefix) {
                set_key_min(lower_key.data(), offset, col);
                set_key_max(upper_key.data(), offset, col);
                offset += col.len;
                continue;
            }

            if (auto eq = find_eq_cond(col)) {
                set_key_part(lower_key.data(), offset, col, eq->rhs_val.raw->data);
                set_key_part(upper_key.data(), offset, col, eq->rhs_val.raw->data);
                has_lower = true;
                has_upper = true;
                offset += col.len;
                continue;
            }

            bool low_inc = true;
            bool up_inc = true;
            auto low = find_lower_cond(col, low_inc);
            auto up = find_upper_cond(col, up_inc);
            if (low != nullptr) {
                set_key_part(lower_key.data(), offset, col, low->rhs_val.raw->data);
                has_lower = true;
                lower_inclusive = low_inc;
            } else {
                set_key_min(lower_key.data(), offset, col);
            }
            if (up != nullptr) {
                set_key_part(upper_key.data(), offset, col, up->rhs_val.raw->data);
                has_upper = true;
                upper_inclusive = up_inc;
            } else {
                set_key_max(upper_key.data(), offset, col);
            }
            offset += col.len;
            stop_prefix = true;
        }

        Iid lower = has_lower
                        ? (lower_inclusive ? ih->lower_bound(lower_key.data()) : ih->upper_bound(lower_key.data()))
                        : ih->leaf_begin();
        Iid upper = has_upper
                        ? (upper_inclusive ? ih->upper_bound(upper_key.data()) : ih->lower_bound(upper_key.data()))
                        : ih->leaf_end();
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec.get(), cols_, fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec.get(), cols_, fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    Rid &rid() override { return rid_; }
};
