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
#include <algorithm>
#include <cstddef>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t JOIN_BUFFER_SIZE = 256 * 1024 * 1024;
    static constexpr size_t RECORD_OVERHEAD = sizeof(RmRecord) + sizeof(std::unique_ptr<RmRecord>) + 32;

    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    std::unique_ptr<RmRecord> curr_rec_;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_pos_;
    size_t max_block_records_;
    std::unique_ptr<RmRecord> right_rec_;

    void load_left_block() {
        left_block_.clear();
        left_pos_ = 0;
        while (!left_->is_end() && left_block_.size() < max_block_records_) {
            left_block_.push_back(left_->Next());
            left_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> make_join_record() {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_block_[left_pos_]->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
        return rec;
    }

    void advance_to_match() {
        curr_rec_ = nullptr;
        while (!left_block_.empty()) {
            while (!right_->is_end()) {
                if (right_rec_ == nullptr) {
                    right_rec_ = right_->Next();
                }
                while (left_pos_ < left_block_.size()) {
                    auto rec = make_join_record();
                    if (eval_conds(rec.get(), cols_, fed_conds_)) {
                        curr_rec_ = std::move(rec);
                        isend = false;
                        return;
                    }
                    ++left_pos_;
                }
                right_->nextTuple();
                right_rec_ = nullptr;
                left_pos_ = 0;
            }
            load_left_block();
            if (!left_block_.empty()) {
                right_->beginTuple();
                right_rec_ = nullptr;
                left_pos_ = 0;
            }
        }
        isend = true;
    }

    void advance_after_current() {
        if (isend) {
            return;
        }
        ++left_pos_;
        advance_to_match();
    }

    void begin_block_join() {
        left_->beginTuple();
        load_left_block();
        if (left_block_.empty()) {
            isend = true;
            curr_rec_ = nullptr;
            return;
        }
        right_->beginTuple();
        right_rec_ = nullptr;
        if (right_->is_end()) {
            isend = true;
            curr_rec_ = nullptr;
            return;
        }
        advance_to_match();
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        left_pos_ = 0;
        max_block_records_ = std::max<size_t>(1, JOIN_BUFFER_SIZE / (left_->tupleLen() + RECORD_OVERHEAD));
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        begin_block_join();
    }

    void nextTuple() override {
        advance_after_current();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (curr_rec_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*curr_rec_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return isend; }

    Rid &rid() override { return _abstract_rid; }
};
