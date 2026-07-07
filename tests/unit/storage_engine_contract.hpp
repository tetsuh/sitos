// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageEngine contract test suite.  Engine implementations include this
// header and instantiate the suite with a factory that returns a fresh engine.
//
// Usage (from tests/unit/ or with the project root in the include path):
//   #include "storage_engine_contract.hpp"
//   INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(MyEngineSuite, [] {
//       return std::make_unique<MyEngine>();
//   });
//
// Required test names (docs/06 §4.1): SnapshotIsIsolatedFromBasePut,
// SnapshotFallbackCopiesForInMemory.

#ifndef SITOS_STORAGE_ENGINE_CONTRACT_HPP
#define SITOS_STORAGE_ENGINE_CONTRACT_HPP

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sitos/storage_engine.hpp"

namespace sitos {
namespace testing {

/// Factory type: returns a freshly constructed engine (each test case gets its
/// own instance so tests do not leak state).
using EngineFactory = std::function<std::unique_ptr<StorageEngine>()>;

/// RAII helper that destroys the engine and then checks expectations on the
/// mock (if any).  Not needed for the contract itself but kept as a convenience.
class EngineContractTest : public ::testing::TestWithParam<EngineFactory> {
 protected:
  void SetUp() override { engine_ = GetParam()(); }
  void TearDown() override { engine_.reset(); }

  StorageEngine& engine() { return *engine_; }

 private:
  std::unique_ptr<StorageEngine> engine_;
};

}  // namespace testing
}  // namespace sitos

// ---------------------------------------------------------------------------
// Contract test bodies (defined as free functions so they can be reused
// across different test fixture / engine combinations).
// ---------------------------------------------------------------------------

namespace sitos_contract {

inline std::vector<std::byte> BytesFromString(std::string_view s) {
  std::vector<std::byte> v;
  v.reserve(s.size());
  for (char c : s) v.push_back(static_cast<std::byte>(c));
  return v;
}

inline void GetReturnsTrueForExistingKey(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("foo", BytesFromString("bar")));
  bool called = false;
  bool ok = engine.Get("foo",
                       [&](std::string_view key, sitos::Bytes value) {
                         called = true;
                         EXPECT_EQ(key, "foo");
                         EXPECT_EQ(value.size(), 3u);
                         EXPECT_EQ(static_cast<char>(value[0]), 'b');
                         return true;
                       });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(called);
}

inline void GetReturnsTrueEvenIfSinkReturnsFalse(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("foo", BytesFromString("bar")));
  bool called = false;
  bool ok = engine.Get("foo",
                       [&](std::string_view /*key*/, sitos::Bytes /*value*/) {
                         called = true;
                         return false;
                       });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(called);
}

inline void GetReturnsFalseForMissingKey(sitos::StorageEngine& engine) {
  bool called = false;
  bool ok = engine.Get("nonexistent",
                       [&](std::string_view /*key*/, sitos::Bytes /*value*/) {
                         called = true;
                         return true;
                       });
  EXPECT_FALSE(ok);
  EXPECT_FALSE(called);
}

inline void GetReturnsFalseForDeletedKey(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("delme", BytesFromString("x")));
  ASSERT_TRUE(engine.Delete("delme"));
  bool ok = engine.Get("delme",
                       [](std::string_view /*key*/, sitos::Bytes /*value*/) { return true; });
  EXPECT_FALSE(ok);
}

inline void PutOverwritesExistingKey(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("key", BytesFromString("old")));
  ASSERT_TRUE(engine.Put("key", BytesFromString("new")));
  bool ok = engine.Get("key",
                       [](std::string_view /*key*/, sitos::Bytes value) {
                         EXPECT_EQ(value.size(), 3u);
                         EXPECT_EQ(static_cast<char>(value[0]), 'n');
                         return true;
                       });
  EXPECT_TRUE(ok);
}

inline void PutEmptyValue(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("empty", std::span<const std::byte>{}));
  bool ok = engine.Get("empty",
                       [](std::string_view /*key*/, sitos::Bytes value) {
                         EXPECT_TRUE(value.empty());
                         return true;
                       });
  EXPECT_TRUE(ok);
}

inline void DeleteNonexistentKeyReturnsTrue(sitos::StorageEngine& engine) {
  // Delete on a non-existent key is a no-op; the convention is to return true
  // (or at least not crash).
  EXPECT_TRUE(engine.Delete("no_such_key"));
}

inline void ListEnumeratesAllEntries(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("a", BytesFromString("1")));
  ASSERT_TRUE(engine.Put("b", BytesFromString("2")));
  ASSERT_TRUE(engine.Put("aa", BytesFromString("3")));

  std::vector<std::pair<std::string, std::string>> entries;
  bool ok = engine.List("",
                        [&](std::string_view key, sitos::Bytes value) {
                          entries.emplace_back(std::string(key),
                                               std::string(reinterpret_cast<const char*>(value.data()),
                                                           value.size()));
                          return true;
                        });
  EXPECT_TRUE(ok);
  EXPECT_EQ(entries.size(), 3u);
}

inline void ListWithPrefixFiltersCorrectly(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("x/1", BytesFromString("a")));
  ASSERT_TRUE(engine.Put("x/2", BytesFromString("b")));
  ASSERT_TRUE(engine.Put("y/1", BytesFromString("c")));

  std::size_t count = 0;
  bool ok = engine.List("x/",
                        [&](std::string_view key, sitos::Bytes /*value*/) {
                          EXPECT_TRUE(key.starts_with("x/"));
                          ++count;
                          return true;
                        });
  EXPECT_TRUE(ok);
  EXPECT_EQ(count, 2u);
}

inline void ListEarlyExitReturnsFalse(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("k1", BytesFromString("v1")));
  ASSERT_TRUE(engine.Put("k2", BytesFromString("v2")));
  ASSERT_TRUE(engine.Put("k3", BytesFromString("v3")));

  int calls = 0;
  bool ok = engine.List("",
                        [&](std::string_view /*key*/, sitos::Bytes /*value*/) {
                          ++calls;
                          return false;  // abort after first entry
                        });
  EXPECT_FALSE(ok);
  EXPECT_EQ(calls, 1);
}

inline void ListEmptyEngine(sitos::StorageEngine& engine) {
  bool called = false;
  bool ok = engine.List("",
                        [&](std::string_view /*key*/, sitos::Bytes /*value*/) {
                          called = true;
                          return true;
                        });
  EXPECT_TRUE(ok);
  EXPECT_FALSE(called);
}

// docs/06 §4.1: SnapshotFallbackCopiesForInMemory
inline void SnapshotFallbackCopiesForInMemory(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("s1", BytesFromString("hello")));
  ASSERT_TRUE(engine.Put("s2", BytesFromString("world")));

  auto snap = engine.TakeSnapshot();
  ASSERT_NE(snap, nullptr);

  // Snapshot should see all keys that existed at the time.
  std::size_t count = 0;
  bool ok = snap->List("",
                       [&](std::string_view /*key*/, sitos::Bytes /*value*/) { ++count; return true; });
  EXPECT_TRUE(ok);
  EXPECT_EQ(count, 2u);

  // Snapshot Get for an existing key.
  bool found = false;
  ok = snap->Get("s1",
                 [&](std::string_view key, sitos::Bytes value) {
                   found = true;
                   EXPECT_EQ(key, "s1");
                   EXPECT_EQ(value.size(), 5u);
                   return true;
                 });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(found);

  // Snapshot Get for a missing key.
  found = false;
  ok = snap->Get("no_such",
                 [&](std::string_view /*key*/, sitos::Bytes /*value*/) { found = true; return true; });
  EXPECT_FALSE(ok);
  EXPECT_FALSE(found);
}

// docs/06 §4.1: SnapshotIsIsolatedFromBasePut
inline void SnapshotIsIsolatedFromBasePut(sitos::StorageEngine& engine) {
  ASSERT_TRUE(engine.Put("iso", BytesFromString("before")));

  auto snap = engine.TakeSnapshot();
  ASSERT_NE(snap, nullptr);

  // Mutate after snapshot.
  ASSERT_TRUE(engine.Put("iso", BytesFromString("after")));
  ASSERT_TRUE(engine.Put("new_key", BytesFromString("new")));

  // Snapshot must still see the old value.
  bool ok = snap->Get("iso",
                      [](std::string_view /*key*/, sitos::Bytes value) {
                        EXPECT_EQ(value.size(), 6u);
                        EXPECT_EQ(static_cast<char>(value[0]), 'b');
                        return true;
                      });
  EXPECT_TRUE(ok);

  // Snapshot must NOT see the new key.
  bool found = false;
  ok = snap->Get("new_key",
                 [&](std::string_view /*key*/, sitos::Bytes /*value*/) { found = true; return true; });
  EXPECT_FALSE(ok);
  EXPECT_FALSE(found);
}

// Verifies that engines treat values as opaque byte sequences.  Values with
// embedded null bytes and arbitrary binary patterns must survive a Put/Get
// round-trip unchanged (no truncation, encoding conversion, or modification).
inline void HandlesOpaqueBytes(sitos::StorageEngine& engine) {
  // Bytes that include embedded nulls (positions 0, 3, last).
  const std::vector<std::byte> with_nulls = {
      std::byte{0x00}, std::byte{0x41}, std::byte{0x42},
      std::byte{0x00}, std::byte{0x43},
      std::byte{0x00},
  };
  ASSERT_TRUE(engine.Put("nulls", with_nulls));
  bool ok = engine.Get("nulls",
                       [&](std::string_view /*key*/, sitos::Bytes value) {
                         EXPECT_EQ(value.size(), with_nulls.size());
                         EXPECT_EQ(std::memcmp(value.data(), with_nulls.data(),
                                               with_nulls.size()),
                                   0);
                         return true;
                       });
  EXPECT_TRUE(ok);

  // Full byte range: 0x00 .. 0xFF.
  std::vector<std::byte> all_bytes(256);
  for (int i = 0; i < 256; ++i) all_bytes[i] = static_cast<std::byte>(i);
  ASSERT_TRUE(engine.Put("all", all_bytes));
  ok = engine.Get("all",
                  [&](std::string_view /*key*/, sitos::Bytes value) {
                    EXPECT_EQ(value.size(), 256u);
                    EXPECT_EQ(std::memcmp(value.data(), all_bytes.data(), 256), 0);
                    return true;
                  });
  EXPECT_TRUE(ok);

  // Non-UTF8 single byte (0xFF alone is invalid UTF-8).
  const std::vector<std::byte> non_utf8 = {std::byte{0xFF}};
  ASSERT_TRUE(engine.Put("non_utf8", non_utf8));
  ok = engine.Get("non_utf8",
                  [&](std::string_view /*key*/, sitos::Bytes value) {
                    EXPECT_EQ(value.size(), 1u);
                    EXPECT_EQ(value[0], std::byte{0xFF});
                    return true;
                  });
  EXPECT_TRUE(ok);
}

}  // namespace sitos_contract

// ---------------------------------------------------------------------------
// Macro to instantiate the full contract suite for a given engine factory.
// Usage (in a .cpp file):
//   #include "tests/unit/storage_engine_contract.hpp"
//   INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(MyTestSuite, [] {
//       return std::make_unique<MyEngine>();
//   });
// ---------------------------------------------------------------------------
#define INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(SuiteName, FactoryExpr)                   \
  class SuiteName : public ::sitos::testing::EngineContractTest {};                         \
                                                                                            \
  TEST_P(SuiteName, GetReturnsTrueForExistingKey) {                                          \
    sitos_contract::GetReturnsTrueForExistingKey(engine());                                  \
  }                                                                                         \
  TEST_P(SuiteName, GetReturnsTrueEvenIfSinkReturnsFalse) {                                  \
    sitos_contract::GetReturnsTrueEvenIfSinkReturnsFalse(engine());                          \
  }                                                                                         \
  TEST_P(SuiteName, GetReturnsFalseForMissingKey) {                                          \
    sitos_contract::GetReturnsFalseForMissingKey(engine());                                  \
  }                                                                                         \
  TEST_P(SuiteName, GetReturnsFalseForDeletedKey) {                                          \
    sitos_contract::GetReturnsFalseForDeletedKey(engine());                                  \
  }                                                                                         \
  TEST_P(SuiteName, PutOverwritesExistingKey) {                                              \
    sitos_contract::PutOverwritesExistingKey(engine());                                      \
  }                                                                                         \
  TEST_P(SuiteName, PutEmptyValue) {                                                         \
    sitos_contract::PutEmptyValue(engine());                                                 \
  }                                                                                         \
  TEST_P(SuiteName, DeleteNonexistentKeyReturnsTrue) {                                       \
    sitos_contract::DeleteNonexistentKeyReturnsTrue(engine());                               \
  }                                                                                         \
  TEST_P(SuiteName, ListEnumeratesAllEntries) {                                               \
    sitos_contract::ListEnumeratesAllEntries(engine());                                      \
  }                                                                                         \
  TEST_P(SuiteName, ListWithPrefixFiltersCorrectly) {                                         \
    sitos_contract::ListWithPrefixFiltersCorrectly(engine());                                \
  }                                                                                         \
  TEST_P(SuiteName, ListEarlyExitReturnsFalse) {                                              \
    sitos_contract::ListEarlyExitReturnsFalse(engine());                                     \
  }                                                                                         \
  TEST_P(SuiteName, ListEmptyEngine) {                                                       \
    sitos_contract::ListEmptyEngine(engine());                                               \
  }                                                                                         \
  TEST_P(SuiteName, SnapshotFallbackCopiesForInMemory) {                                     \
    sitos_contract::SnapshotFallbackCopiesForInMemory(engine());                             \
  }                                                                                         \
  TEST_P(SuiteName, SnapshotIsIsolatedFromBasePut) {                                         \
    sitos_contract::SnapshotIsIsolatedFromBasePut(engine());                                 \
  }                                                                                         \
  TEST_P(SuiteName, HandlesOpaqueBytes) {                                                    \
    sitos_contract::HandlesOpaqueBytes(engine());                                            \
  }                                                                                         \
                                                                                            \
  INSTANTIATE_TEST_SUITE_P(, SuiteName, ::testing::Values(FactoryExpr))

#endif  // SITOS_STORAGE_ENGINE_CONTRACT_HPP
