/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::compatible(LockMode held, LockMode requested) {
    if (held == LockMode::INTENTION_SHARED) {
        return requested != LockMode::EXLUCSIVE;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED || requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::S_IX) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

LockManager::GroupLockMode LockManager::get_group_lock_mode(const std::list<LockRequest> &request_queue) {
    bool has_is = false;
    bool has_ix = false;
    bool has_s = false;
    bool has_x = false;
    for (const auto &request : request_queue) {
        if (!request.granted_) {
            continue;
        }
        has_is = has_is || request.lock_mode_ == LockMode::INTENTION_SHARED;
        has_ix = has_ix || request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE;
        has_s = has_s || request.lock_mode_ == LockMode::SHARED;
        has_x = has_x || request.lock_mode_ == LockMode::EXLUCSIVE;
    }
    if (has_x) {
        return GroupLockMode::X;
    }
    if (has_s && has_ix) {
        return GroupLockMode::SIX;
    }
    if (has_s) {
        return GroupLockMode::S;
    }
    if (has_ix) {
        return GroupLockMode::IX;
    }
    if (has_is) {
        return GroupLockMode::IS;
    }
    return GroupLockMode::NON_LOCK;
}

bool LockManager::lock(Transaction* txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lock_guard(latch_);
    auto &request_queue = lock_table_[lock_data_id];
    for (auto iter = request_queue.request_queue_.begin(); iter != request_queue.request_queue_.end(); ++iter) {
        if (iter->txn_id_ == txn->get_transaction_id()) {
            if (iter->lock_mode_ == lock_mode || iter->lock_mode_ == LockMode::EXLUCSIVE ||
                (iter->lock_mode_ == LockMode::SHARED && lock_mode == LockMode::INTENTION_SHARED) ||
                (iter->lock_mode_ == LockMode::INTENTION_EXCLUSIVE && lock_mode == LockMode::INTENTION_SHARED)) {
                return true;
            }

            for (const auto &other : request_queue.request_queue_) {
                if (other.txn_id_ != txn->get_transaction_id() && other.granted_ &&
                    !compatible(other.lock_mode_, lock_mode)) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
            iter->lock_mode_ = lock_mode;
            iter->granted_ = true;
            request_queue.group_lock_mode_ = get_group_lock_mode(request_queue.request_queue_);
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
    }

    for (const auto &request : request_queue.request_queue_) {
        if (request.granted_ && !compatible(request.lock_mode_, lock_mode)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    request_queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    request_queue.request_queue_.back().granted_ = true;
    request_queue.group_lock_mode_ = get_group_lock_mode(request_queue.request_queue_);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    std::unique_lock<std::mutex> lock_guard(latch_);
    auto table_iter = lock_table_.find(lock_data_id);
    if (table_iter == lock_table_.end()) {
        return false;
    }

    auto &request_queue = table_iter->second;
    for (auto iter = request_queue.request_queue_.begin(); iter != request_queue.request_queue_.end(); ++iter) {
        if (iter->txn_id_ == txn->get_transaction_id()) {
            request_queue.request_queue_.erase(iter);
            break;
        }
    }
    request_queue.group_lock_mode_ = get_group_lock_mode(request_queue.request_queue_);
    if (request_queue.request_queue_.empty()) {
        lock_table_.erase(table_iter);
    }
    txn->get_lock_set()->erase(lock_data_id);
    txn->set_state(TransactionState::SHRINKING);
    return true;
}
