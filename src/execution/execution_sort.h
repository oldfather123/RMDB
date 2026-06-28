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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SortKey> sort_keys_;
    std::vector<size_t> sort_idxs_;
    std::vector<ColMeta> cols_;
    size_t len_;
    int limit_;
    bool loaded_;
    size_t cursor_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;

    void load_and_sort() {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        tuples_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < sort_idxs_.size(); ++i) {
                const auto &col = cols_[sort_idxs_[i]];
                int cmp = compare_raw(lhs->data + col.offset, rhs->data + col.offset, col.type, col.len);
                if (cmp == 0) {
                    continue;
                }
                return sort_keys_[i].is_desc ? cmp > 0 : cmp < 0;
            }
            return false;
        });
        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(static_cast<size_t>(limit_));
        }
    }

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SortKey> sort_keys, int limit) {
        prev_ = std::move(prev);
        sort_keys_ = std::move(sort_keys);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        limit_ = limit;
        loaded_ = false;
        cursor_ = 0;
        for (auto &key : sort_keys_) {
            auto pos = get_col(cols_, key.col);
            sort_idxs_.push_back(pos - cols_.begin());
        }
    }

    void beginTuple() override { 
        load_and_sort();
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    Rid &rid() override { return _abstract_rid; }
};
