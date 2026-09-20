// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
#define SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitos/rocksdb_engine.hpp"

namespace sitos::rocksdb_test {

class EventLog final {
 public:
  void Add(std::string_view event) noexcept {
    try {
      std::string copy(event);
      std::lock_guard lock(mutex_);
      events_.push_back(std::move(copy));
    } catch (...) {
      // Test instrumentation must never interrupt product cleanup.
    }
  }

  std::vector<std::string> Snapshot() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

inline constexpr unsigned int kPut = 1U << 0;
inline constexpr unsigned int kDelete = 1U << 1;
inline constexpr unsigned int kGet = 1U << 2;
inline constexpr unsigned int kList = 1U << 3;

enum class SyncFailureMode {
  kNone,
  kBeforeInvocation,
  kNativeStatus,
  kNativeException,
};

enum class MutationOperation {
  kPut,
  kDelete,
  kSync,
};

struct WriteObservation {
  std::size_t invocation_count = 0;
  bool disable_wal = false;
  bool sync = false;
};

struct SyncObservation {
  std::size_t invocation_count = 0;
  bool last_sync_argument = false;
};

class OperationBlock final {
 public:
  bool WaitUntilEntered();
  void Release();

 private:
  friend void WaitOnOperationBlock(const std::shared_ptr<OperationBlock>& block);
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

void WaitOnOperationBlock(const std::shared_ptr<OperationBlock>& block);
void SetOpenFailureForTest();
void SetSnapshotReleaseFailureForTest(const RocksDBEngine& engine);
void SetFailures(const RocksDBEngine& engine, unsigned int failures);
void SetSyncFailureModeForTest(const RocksDBEngine& engine, SyncFailureMode mode);
std::shared_ptr<OperationBlock> BlockNextMutationForTest(const RocksDBEngine& engine,
                                                         MutationOperation operation);
void GetWriteObservationsForTest(const RocksDBEngine& engine, WriteObservation& put,
                                 WriteObservation& delete_key);
SyncObservation GetSyncObservationForTest(const RocksDBEngine& engine);
void GetSnapshotStats(const RocksDBEngine& engine, std::size_t& snapshot_calls,
                      std::size_t& enumeration_calls);
std::shared_ptr<EventLog> GetEventLog(const RocksDBEngine& engine);

}  // namespace sitos::rocksdb_test

#endif  // SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
