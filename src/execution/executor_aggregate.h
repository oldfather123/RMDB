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

#include "execution_defs.h"
#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<AggregateCall> aggs_;
    std::vector<ColMeta> cols_;
    std::vector<size_t> src_idxs_;
    size_t len_;
    bool computed_ = false;
    bool emitted_ = false;
    std::unique_ptr<RmRecord> result_;

    ColType result_type(const AggregateCall &agg, const ColMeta *src_col) {
        if (agg.type == AGG_COUNT) {
            return TYPE_INT;
        }
        if (agg.type == AGG_SUM && src_col->type == TYPE_INT) {
            return TYPE_INT;
        }
        return src_col->type;
    }

    int result_len(ColType type, const ColMeta *src_col) {
        if (type == TYPE_INT) {
            return sizeof(int);
        }
        if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            return sizeof(int64_t);
        }
        if (type == TYPE_FLOAT) {
            return sizeof(float);
        }
        return src_col->len;
    }

    void write_zero(char *dst, ColType type, int len) {
        memset(dst, 0, len);
        if (type == TYPE_INT) {
            *(int *)dst = 0;
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            *(int64_t *)dst = 0;
        } else if (type == TYPE_FLOAT) {
            *(float *)dst = 0.0f;
        }
    }

    void compute() {
        if (computed_) {
            return;
        }
        computed_ = true;
        result_ = std::make_unique<RmRecord>(len_);
        std::vector<bool> has_value(aggs_.size(), false);
        std::vector<int> counts(aggs_.size(), 0);
        std::vector<double> sums(aggs_.size(), 0.0);

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            for (size_t i = 0; i < aggs_.size(); ++i) {
                const auto &agg = aggs_[i];
                auto &dst_col = cols_[i];
                char *dst = result_->data + dst_col.offset;
                if (agg.type == AGG_COUNT) {
                    counts[i]++;
                    continue;
                }

                const auto &src_col = prev_->cols()[src_idxs_[i]];
                const char *src = rec->data + src_col.offset;
                if (agg.type == AGG_SUM) {
                    if (src_col.type == TYPE_INT) {
                        sums[i] += *(int *)src;
                    } else {
                        sums[i] += *(float *)src;
                    }
                    has_value[i] = true;
                } else if (!has_value[i] || compare_raw(src, dst, src_col.type, src_col.len) *
                                               (agg.type == AGG_MAX ? 1 : -1) > 0) {
                    memcpy(dst, src, src_col.len);
                    has_value[i] = true;
                }
            }
        }

        for (size_t i = 0; i < aggs_.size(); ++i) {
            char *dst = result_->data + cols_[i].offset;
            if (aggs_[i].type == AGG_COUNT) {
                *(int *)dst = counts[i];
            } else if (aggs_[i].type == AGG_SUM) {
                if (cols_[i].type == TYPE_INT) {
                    *(int *)dst = static_cast<int>(sums[i]);
                } else {
                    *(float *)dst = static_cast<float>(sums[i]);
                }
            } else if (!has_value[i]) {
                write_zero(dst, cols_[i].type, cols_[i].len);
            }
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<AggregateCall> &aggs) {
        prev_ = std::move(prev);
        aggs_ = aggs;

        size_t curr_offset = 0;
        const auto &prev_cols = prev_->cols();
        for (auto &agg : aggs_) {
            const ColMeta *src_col = nullptr;
            if (!agg.is_star) {
                auto pos = get_col(prev_cols, agg.col);
                src_idxs_.push_back(pos - prev_cols.begin());
                src_col = &(*pos);
            } else {
                src_idxs_.push_back(0);
            }
            ColMeta col;
            col.tab_name = "";
            col.name = agg.alias;
            col.type = result_type(agg, src_col);
            col.len = result_len(col.type, src_col);
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override {
        compute();
        emitted_ = false;
    }

    void nextTuple() override { emitted_ = true; }

    std::unique_ptr<RmRecord> Next() override {
        compute();
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, result_->data, len_);
        return rec;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return emitted_; }

    Rid &rid() override { return _abstract_rid; }
};
