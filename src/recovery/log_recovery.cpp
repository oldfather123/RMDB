/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <algorithm>
#include <cstring>

namespace {
std::string table_name(const char *name, size_t size) {
    return std::string(name, size);
}

std::unique_ptr<LogRecord> make_log_record(LogType type) {
    switch (type) {
        case LogType::UPDATE:
            return std::make_unique<UpdateLogRecord>();
        case LogType::INSERT:
            return std::make_unique<InsertLogRecord>();
        case LogType::DELETE:
            return std::make_unique<DeleteLogRecord>();
        case LogType::begin:
            return std::make_unique<BeginLogRecord>();
        case LogType::commit:
            return std::make_unique<CommitLogRecord>();
        case LogType::ABORT:
            return std::make_unique<AbortLogRecord>();
        default:
            return nullptr;
    }
}

std::unique_ptr<char[]> make_index_key(const RmRecord &record, const IndexMeta &index) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    int offset = 0;
    for (size_t i = 0; i < static_cast<size_t>(index.col_num); ++i) {
        memcpy(key.get() + offset, record.data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

bool record_exists(RmFileHandle *fh, const Rid &rid) {
    try {
        return fh->is_record(rid);
    } catch (RMDBError &) {
        return false;
    }
}

bool same_record(RmFileHandle *fh, const Rid &rid, const RmRecord &record) {
    try {
        auto current = fh->get_record(rid, nullptr);
        return current->size == record.size && memcmp(current->data, record.data, record.size) == 0;
    } catch (RMDBError &) {
        return false;
    }
}

void delete_indexes(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record) {
    auto &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(record, index);
        ih->delete_entry(key.get(), nullptr);
    }
}

void insert_indexes(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record, const Rid &rid) {
    auto &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(record, index);
        ih->delete_entry(key.get(), nullptr);
        ih->insert_entry(key.get(), rid, nullptr);
    }
}

void redo_insert(SmManager *sm_manager, InsertLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    if (!same_record(fh, log->rid_, log->insert_value_)) {
        if (record_exists(fh, log->rid_)) {
            try {
                auto old = fh->get_record(log->rid_, nullptr);
                delete_indexes(sm_manager, tab_name, *old);
                fh->delete_record(log->rid_, nullptr);
            } catch (RMDBError &) {
            }
        }
        fh->insert_record(log->rid_, log->insert_value_.data);
    }
    insert_indexes(sm_manager, tab_name, log->insert_value_, log->rid_);
}

void redo_delete(SmManager *sm_manager, DeleteLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    delete_indexes(sm_manager, tab_name, log->delete_value_);
    if (record_exists(fh, log->rid_)) {
        try {
            fh->delete_record(log->rid_, nullptr);
        } catch (RMDBError &) {
        }
    }
}

void redo_update(SmManager *sm_manager, UpdateLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    if (record_exists(fh, log->rid_)) {
        try {
            auto current = fh->get_record(log->rid_, nullptr);
            delete_indexes(sm_manager, tab_name, *current);
        } catch (RMDBError &) {
        }
        try {
            fh->update_record(log->rid_, log->new_value_.data, nullptr);
        } catch (RMDBError &) {
            fh->insert_record(log->rid_, log->new_value_.data);
        }
    } else {
        fh->insert_record(log->rid_, log->new_value_.data);
    }
    insert_indexes(sm_manager, tab_name, log->new_value_, log->rid_);
}

void undo_insert(SmManager *sm_manager, InsertLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    delete_indexes(sm_manager, tab_name, log->insert_value_);
    if (record_exists(fh, log->rid_)) {
        try {
            fh->delete_record(log->rid_, nullptr);
        } catch (RMDBError &) {
        }
    }
}

void undo_delete(SmManager *sm_manager, DeleteLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    if (!same_record(fh, log->rid_, log->delete_value_)) {
        if (record_exists(fh, log->rid_)) {
            try {
                auto current = fh->get_record(log->rid_, nullptr);
                delete_indexes(sm_manager, tab_name, *current);
                fh->delete_record(log->rid_, nullptr);
            } catch (RMDBError &) {
            }
        }
        fh->insert_record(log->rid_, log->delete_value_.data);
    }
    insert_indexes(sm_manager, tab_name, log->delete_value_, log->rid_);
}

void undo_update(SmManager *sm_manager, UpdateLogRecord *log) {
    std::string tab_name = table_name(log->table_name_, log->table_name_size_);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    if (record_exists(fh, log->rid_)) {
        try {
            auto current = fh->get_record(log->rid_, nullptr);
            delete_indexes(sm_manager, tab_name, *current);
        } catch (RMDBError &) {
        }
        try {
            fh->update_record(log->rid_, log->old_value_.data, nullptr);
        } catch (RMDBError &) {
            fh->insert_record(log->rid_, log->old_value_.data);
        }
    } else {
        fh->insert_record(log->rid_, log->old_value_.data);
    }
    insert_indexes(sm_manager, tab_name, log->old_value_, log->rid_);
}
}  // namespace

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    logs_.clear();
    txn_logs_.clear();
    committed_txns_.clear();
    ended_txns_.clear();

    int offset = 0;
    while (true) {
        char header[LOG_HEADER_SIZE];
        int bytes = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (bytes < LOG_HEADER_SIZE) {
            break;
        }
        auto type = *reinterpret_cast<LogType *>(header + OFFSET_LOG_TYPE);
        uint32_t len = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (len < LOG_HEADER_SIZE || len > LOG_BUFFER_SIZE) {
            break;
        }
        std::vector<char> data(len);
        bytes = disk_manager_->read_log(data.data(), len, offset);
        if (bytes < static_cast<int>(len)) {
            break;
        }
        auto log = make_log_record(type);
        if (log == nullptr) {
            break;
        }
        log->deserialize(data.data());
        LogRecord *raw = log.get();
        logs_.push_back(std::move(log));
        txn_logs_[raw->log_tid_].push_back(raw);
        if (raw->log_type_ == LogType::commit) {
            committed_txns_.insert(raw->log_tid_);
            ended_txns_.insert(raw->log_tid_);
        } else if (raw->log_type_ == LogType::ABORT) {
            ended_txns_.insert(raw->log_tid_);
        }
        offset += len;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for (auto &log : logs_) {
        if (!committed_txns_.count(log->log_tid_)) {
            continue;
        }
        try {
            if (log->log_type_ == LogType::INSERT) {
                redo_insert(sm_manager_, static_cast<InsertLogRecord *>(log.get()));
            } else if (log->log_type_ == LogType::DELETE) {
                redo_delete(sm_manager_, static_cast<DeleteLogRecord *>(log.get()));
            } else if (log->log_type_ == LogType::UPDATE) {
                redo_update(sm_manager_, static_cast<UpdateLogRecord *>(log.get()));
            }
        } catch (RMDBError &) {
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    for (auto &entry : txn_logs_) {
        txn_id_t txn_id = entry.first;
        if (ended_txns_.count(txn_id)) {
            continue;
        }
        auto &logs = entry.second;
        for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
            LogRecord *log = *it;
            try {
                if (log->log_type_ == LogType::INSERT) {
                    undo_insert(sm_manager_, static_cast<InsertLogRecord *>(log));
                } else if (log->log_type_ == LogType::DELETE) {
                    undo_delete(sm_manager_, static_cast<DeleteLogRecord *>(log));
                } else if (log->log_type_ == LogType::UPDATE) {
                    undo_update(sm_manager_, static_cast<UpdateLogRecord *>(log));
                }
            } catch (RMDBError &) {
            }
        }
    }
    buffer_pool_manager_->flush_all_pages(-1);
}
