// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
#define SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitos/rocksdb_engine.hpp"

namespace sitos::rocksdb_test {

struct EventLog {
  void Add(std::string_view event) noexcept {
    try {
      std::string copy(event);
      std::lock_guard lock(mutex);
      events.push_back(std::move(copy));
    } catch (...) {
    }
  }

  mutable std::mutex mutex;
  std::vector<std::string> events;
};

inline constexpr unsigned int kPut = 1U << 0;
inline constexpr unsigned int kDelete = 1U << 1;
inline constexpr unsigned int kGet = 1U << 2;
inline constexpr unsigned int kList = 1U << 3;

void SetOpenFailureForTest();
void SetFailures(RocksDBEngine& engine, unsigned int failures);
void GetSnapshotStats(const RocksDBEngine& engine, std::size_t& snapshot_calls,
                      std::size_t& enumeration_calls);
std::shared_ptr<EventLog> GetEventLog(const RocksDBEngine& engine);

}  // namespace sitos::rocksdb_test

#endif  // SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
