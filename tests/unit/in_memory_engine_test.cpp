// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// InMemoryEngine tests: contract suite + concurrent stress test.

#include "storage_engine_contract.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "sitos/in_memory_engine.hpp"

// ---------------------------------------------------------------------------
// Contract suite
// ---------------------------------------------------------------------------

INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(InMemoryEngineContractTest, [] {
  return std::make_unique<sitos::InMemoryEngine>();
});

// ---------------------------------------------------------------------------
// Concurrent read/write stress test (TSan-ready).
// ---------------------------------------------------------------------------

TEST(InMemoryEngineStressTest, ConcurrentPutGet) {
  sitos::InMemoryEngine engine;

  constexpr int kNumWriters = 4;
  constexpr int kNumReaders = 4;
  constexpr int kOpsPerThread = 1000;
  std::atomic<bool> start{false};
  std::atomic<int> errors{0};

  for (int id = 0; id < kNumWriters; ++id) {
    ASSERT_TRUE(engine.Put("list/" + std::to_string(id), {}));
  }

  auto writer = [&](int id) {
    while (!start.load()) { /* spin */ }
    for (int i = 0; i < kOpsPerThread; ++i) {
      auto key = "key_" + std::to_string(id) + "_" + std::to_string(i);
      std::vector<std::byte> value{std::byte{static_cast<unsigned char>(id)},
                                   std::byte{static_cast<unsigned char>(i & 0xFF)}};
      if (!engine.Put(key, value)) ++errors;
      if (!engine.Put("list/" + std::to_string(id), value)) ++errors;
    }
  };

  auto reader = [&]() {
    while (!start.load()) { /* spin */ }
    for (int i = 0; i < kOpsPerThread; ++i) {
      engine.Get("no_such_key_" + std::to_string(i),
                 [](std::string_view /*k*/, sitos::Bytes /*v*/) { return true; });
      // This matching prefix has a fixed small cardinality, so readers exercise
      // concurrent entry copying without repeatedly copying the growing map.
      engine.List("list/",
                  [](std::string_view /*k*/, sitos::Bytes /*v*/) { return true; });
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumWriters + kNumReaders);
  for (int i = 0; i < kNumWriters; ++i) threads.emplace_back(writer, i);
  for (int i = 0; i < kNumReaders; ++i) threads.emplace_back(reader);

  start.store(true);
  for (auto& t : threads) t.join();

  EXPECT_EQ(errors.load(), 0);

  // Verify all keys are persisted.
  std::size_t count = 0;
  engine.List("", [&count](std::string_view /*k*/, sitos::Bytes /*v*/) {
    ++count;
    return true;
  });
  EXPECT_EQ(count, static_cast<std::size_t>(kNumWriters * kOpsPerThread + kNumWriters));
}

TEST(InMemoryEngineStressTest, SnapshotReadDuringWrite) {
  sitos::InMemoryEngine engine;

  // Pre-populate.
  for (int i = 0; i < 100; ++i) {
    engine.Put("pre_" + std::to_string(i), {});
  }

  auto snap = engine.TakeSnapshot();
  ASSERT_NE(snap, nullptr);

  std::atomic<bool> done{false};

  // Writer keeps modifying after snapshot.
  std::thread writer([&]() {
    for (int i = 0; i < 500 && !done.load(); ++i) {
      engine.Put("post_" + std::to_string(i),
                 std::vector<std::byte>{std::byte{static_cast<unsigned char>(i)}});
      engine.Delete("pre_" + std::to_string(i % 100));
    }
  });

  // Snapshot reads should see consistent pre-populated state.
  std::size_t snap_count = 0;
  snap->List("", [&snap_count](std::string_view /*k*/, sitos::Bytes /*v*/) {
    ++snap_count;
    return true;
  });
  EXPECT_EQ(snap_count, 100u);  // Only pre_ keys.

  bool ok = snap->Get("pre_0",
                      [](std::string_view /*k*/, sitos::Bytes /*v*/) { return true; });
  EXPECT_TRUE(ok);

  ok = snap->Get("post_0", [](std::string_view /*k*/, sitos::Bytes /*v*/) { return true; });
  EXPECT_FALSE(ok);

  done.store(true);
  writer.join();
}
