// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "sitos/rocksdb_engine.hpp"
#include "rocksdb_engine_test_access.hpp"

namespace {

std::filesystem::path MakeSeamTestPath() {
  static std::atomic<unsigned int> next_id{0};
  const auto id = next_id.fetch_add(1);
#if defined(_WIN32)
  const auto process_id = _getpid();
#else
  const auto process_id = getpid();
#endif
  const auto path = std::filesystem::temp_directory_path() /
                    ("sitos-rocksdb-seam-test-" + std::to_string(process_id) + "-" +
                     std::to_string(id));
  std::error_code error;
  std::filesystem::remove_all(path, error);
  EXPECT_FALSE(error);
  return path;
}

class EventCleanupToken final {
 public:
  EventCleanupToken(std::filesystem::path path,
                    std::shared_ptr<sitos::rocksdb_test::EventLog> events)
      : path_(std::move(path)), events_(std::move(events)) {}

  ~EventCleanupToken() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (events_) events_->Add(error ? "directory_remove_failed" : "directory_remove");
  }

 private:
  std::filesystem::path path_;
  std::shared_ptr<sitos::rocksdb_test::EventLog> events_;
};

class SeamSnapshotAdapter final : public sitos::StorageReader {
 public:
  SeamSnapshotAdapter(std::shared_ptr<const sitos::StorageReader> snapshot,
                      std::shared_ptr<EventCleanupToken> cleanup)
      : cleanup_(std::move(cleanup)), snapshot_(std::move(snapshot)) {}

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    return snapshot_->Get(key, sink);
  }
  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    return snapshot_->List(prefix, sink);
  }

 private:
  std::shared_ptr<EventCleanupToken> cleanup_;
  std::shared_ptr<const sitos::StorageReader> snapshot_;
};

}  // namespace

TEST(RocksDBEngineTestSeam, InjectedNativeOpenFailureReturnsError) {
  const auto path = MakeSeamTestPath();
  sitos::rocksdb_test::SetOpenFailureForTest();
  const auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  EXPECT_TRUE(result.Error());
  EXPECT_FALSE(std::filesystem::exists(path));

  auto retry = sitos::RocksDBEngine::Open(path.string());
  ASSERT_TRUE(retry.IsOk());
  auto engine = std::move(retry).Value();
  ASSERT_NE(engine, nullptr);
  engine.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  EXPECT_FALSE(error);
}

TEST(RocksDBEngineTestSeam, TakeSnapshotUsesNativePrimitiveWithoutEnumeration) {
  const auto path = MakeSeamTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_TRUE(result.IsOk());
  auto engine = std::move(result).Value();
  ASSERT_TRUE(engine->Put("key", std::vector<std::byte>{std::byte{0x01}}));
  auto snapshot = engine->TakeSnapshot();
  ASSERT_NE(snapshot, nullptr);
  std::size_t snapshot_calls = 0;
  std::size_t enumeration_calls = 0;
  sitos::rocksdb_test::GetSnapshotStats(*engine, snapshot_calls, enumeration_calls);
  EXPECT_EQ(snapshot_calls, 1u);
  EXPECT_EQ(enumeration_calls, 0u);
  std::error_code error;
  snapshot.reset();
  engine.reset();
  std::filesystem::remove_all(path, error);
  EXPECT_FALSE(error);
}

TEST(RocksDBEngineTestSeam, SnapshotOutlivesEngineWithOrderedCleanup) {
  const auto path = MakeSeamTestPath();
  std::shared_ptr<const sitos::StorageReader> snapshot;
  std::shared_ptr<sitos::rocksdb_test::EventLog> events;
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    events = sitos::rocksdb_test::GetEventLog(*engine);
    auto cleanup = std::make_shared<EventCleanupToken>(path, events);
    ASSERT_TRUE(engine->Put("stable", std::vector<std::byte>{std::byte{0x01}}));
    snapshot = std::make_shared<SeamSnapshotAdapter>(engine->TakeSnapshot(), cleanup);
    ASSERT_NE(snapshot, nullptr);
    engine.reset();
    EXPECT_TRUE(snapshot->Get("stable", [](std::string_view, sitos::Bytes value) {
      return value.size() == 1 && value[0] == std::byte{0x01};
    }));
  }
  auto recorded = events->Snapshot();
  ASSERT_EQ(recorded.size(), 2u);
  EXPECT_EQ(recorded[0], "open");
  EXPECT_EQ(recorded[1], "get_snapshot");
  snapshot.reset();
  recorded = events->Snapshot();
  ASSERT_EQ(recorded.size(), 5u);
  EXPECT_EQ(recorded[0], "open");
  EXPECT_EQ(recorded[1], "get_snapshot");
  EXPECT_EQ(recorded[2], "release_snapshot");
  EXPECT_EQ(recorded[3], "db_close");
  EXPECT_EQ(recorded[4], "directory_remove");
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(RocksDBEngineTestSeam, InjectedReleaseFailureFallsBackWithoutTerminating) {
  const auto path = MakeSeamTestPath();
  std::shared_ptr<const sitos::StorageReader> snapshot;
  std::shared_ptr<sitos::rocksdb_test::EventLog> events;
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    events = sitos::rocksdb_test::GetEventLog(*engine);
    auto cleanup = std::make_shared<EventCleanupToken>(path, events);
    snapshot = std::make_shared<SeamSnapshotAdapter>(engine->TakeSnapshot(), cleanup);
    sitos::rocksdb_test::SetSnapshotReleaseFailureForTest(*engine);
    engine.reset();
  }

  snapshot.reset();
  const auto recorded = events->Snapshot();
  ASSERT_EQ(recorded.size(), 6u);
  EXPECT_EQ(recorded[0], "open");
  EXPECT_EQ(recorded[1], "get_snapshot");
  EXPECT_EQ(recorded[2], "release_snapshot_recovered");
  EXPECT_EQ(recorded[3], "release_snapshot");
  EXPECT_EQ(recorded[4], "db_close");
  EXPECT_EQ(recorded[5], "directory_remove");
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(RocksDBEngineTestSeam, NativeFailuresPreserveStorageContracts) {
  const auto path = MakeSeamTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_TRUE(result.IsOk());
  auto engine = std::move(result).Value();
  ASSERT_TRUE(engine->Put("fault", std::vector<std::byte>{std::byte{0x01}}));

  sitos::rocksdb_test::SetFailures(*engine, sitos::rocksdb_test::kPut);
  EXPECT_FALSE(engine->Put("fault", std::vector<std::byte>{std::byte{0x02}}));
  EXPECT_TRUE(engine->Get("fault", [](std::string_view, sitos::Bytes value) {
    return value.size() == 1 && value[0] == std::byte{0x01};
  }));
  sitos::rocksdb_test::SetFailures(*engine, sitos::rocksdb_test::kDelete);
  EXPECT_FALSE(engine->Delete("fault"));

  bool called = false;
  sitos::rocksdb_test::SetFailures(*engine, sitos::rocksdb_test::kGet);
  EXPECT_FALSE(engine->Get("fault", [&](std::string_view, sitos::Bytes) {
    called = true;
    return true;
  }));
  EXPECT_FALSE(called);
  sitos::rocksdb_test::SetFailures(*engine, sitos::rocksdb_test::kList);
  EXPECT_FALSE(engine->List("", [&](std::string_view, sitos::Bytes) {
    called = true;
    return true;
  }));
  EXPECT_FALSE(called);

  sitos::rocksdb_test::SetFailures(*engine, 0);
  engine.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  EXPECT_FALSE(error);
}
