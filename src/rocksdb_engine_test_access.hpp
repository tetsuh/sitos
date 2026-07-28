// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
#define SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP

#include "sitos/rocksdb_engine.hpp"

namespace sitos {

/// Source-test-only controls for deterministic native failure contract tests.
class RocksDBEngineTestAccess {
 public:
  enum Failure : unsigned int {
    kPut = 1U << 0,
    kDelete = 1U << 1,
    kGet = 1U << 2,
    kList = 1U << 3,
  };

  static void SetFailures(RocksDBEngine& engine, unsigned int failures) {
    engine.SetFailureMaskForTest(failures);
  }

  static void GetSnapshotStats(const RocksDBEngine& engine, std::size_t& snapshot_calls,
                               std::size_t& enumeration_calls) {
    engine.GetSnapshotStatsForTest(snapshot_calls, enumeration_calls);
  }
};

}  // namespace sitos

#endif  // SITOS_ROCKSDB_ENGINE_TEST_ACCESS_HPP
