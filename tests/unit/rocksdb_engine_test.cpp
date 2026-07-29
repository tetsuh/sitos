// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "storage_engine_contract.hpp"

#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
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

std::filesystem::path MakeTestPath() {
  static std::atomic<unsigned int> next_id{0};
  const auto id = next_id.fetch_add(1);
#if defined(_WIN32)
  const auto process_id = _getpid();
#else
  const auto process_id = getpid();
#endif
  return std::filesystem::temp_directory_path() /
         ("sitos-rocksdb-test-" + std::to_string(process_id) + "-" + std::to_string(id));
}

class CleanupToken final {
 public:
  CleanupToken(std::filesystem::path path, std::shared_ptr<sitos::rocksdb_test::EventLog> events)
      : path_(std::move(path)), events_(std::move(events)) {}

  ~CleanupToken() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    events_->Add(error ? "directory_remove_failed" : "directory_remove");
  }

 private:
  std::filesystem::path path_;
  std::shared_ptr<sitos::rocksdb_test::EventLog> events_;
};

class SnapshotAdapter final : public sitos::StorageReader {
 public:
  SnapshotAdapter(std::shared_ptr<const sitos::StorageReader> snapshot,
                  std::shared_ptr<CleanupToken> cleanup)
      : cleanup_(std::move(cleanup)), snapshot_(std::move(snapshot)) {}

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    return snapshot_->Get(key, sink);
  }
  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    return snapshot_->List(prefix, sink);
  }

 private:
  std::shared_ptr<CleanupToken> cleanup_;
  std::shared_ptr<const sitos::StorageReader> snapshot_;
};

class ContractEngine final : public sitos::StorageEngine {
 public:
  ContractEngine(std::unique_ptr<sitos::RocksDBEngine> engine,
                 std::shared_ptr<CleanupToken> cleanup)
      : cleanup_(std::move(cleanup)), engine_(std::move(engine)) {}

  ~ContractEngine() override {
    engine_.reset();
    cleanup_.reset();
  }

  bool Put(std::string_view key, sitos::Bytes value) override { return engine_->Put(key, value); }
  bool Delete(std::string_view key) override { return engine_->Delete(key); }
  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    return engine_->Get(key, sink);
  }
  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    return engine_->List(prefix, sink);
  }
  std::shared_ptr<const sitos::StorageReader> TakeSnapshot() const override {
    auto snapshot = engine_->TakeSnapshot();
    if (snapshot == nullptr) return nullptr;
    return std::make_shared<SnapshotAdapter>(std::move(snapshot), cleanup_);
  }

 private:
  std::shared_ptr<CleanupToken> cleanup_;
  std::unique_ptr<sitos::RocksDBEngine> engine_;
};

std::unique_ptr<sitos::StorageEngine> MakeContractEngine() {
  const auto path = MakeTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  if (!result.IsOk()) {
    ADD_FAILURE() << "RocksDBEngine::Open failed: status="
                  << static_cast<int>(result.StatusCode()) << ", message=" << result.Message()
                  << ", cause=" << result.Error().category().name() << ":"
                  << result.Error().value();
    return nullptr;
  }
  auto engine = std::move(result).Value();
  auto events = sitos::rocksdb_test::GetEventLog(*engine);
  return std::make_unique<ContractEngine>(
      std::move(engine), std::make_shared<CleanupToken>(path, std::move(events)));
}

}  // namespace

INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(RocksDBEngineContractTest, MakeContractEngine);

TEST(RocksDBEngineOpenApi, EmptyPathIsInvalidArgument) {
  const auto result = sitos::RocksDBEngine::Open("");
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_TRUE(result.Error());
}

TEST(RocksDBEngineOpenApi, ExistingFilePathReturnsNativeErrorWithoutValueBytes) {
  const auto path = MakeTestPath();
  {
    std::ofstream file(path);
    file << "not-a-database";
  }
  const auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  EXPECT_TRUE(result.Error());
  EXPECT_EQ(result.Message().find("not-a-database"), std::string_view::npos);
  std::error_code error;
  std::filesystem::remove(path, error);
}

TEST(RocksDBEngineOpenApi, CreatesAndReopensPersistentDatabase) {
  const auto path = MakeTestPath();
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    ASSERT_TRUE(engine->Put("persist", std::vector<std::byte>{std::byte{0x42}}));
  }
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    EXPECT_TRUE(engine->Get("persist", [](std::string_view, sitos::Bytes value) {
      return value.size() == 1 && value[0] == std::byte{0x42};
    }));
  }
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

TEST(RocksDBEngineSnapshotLifetime, TakeSnapshotUsesNativePrimitiveWithoutEnumeration) {
  const auto path = MakeTestPath();
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
}

TEST(RocksDBEngineSnapshotLifetime, SnapshotOutlivesEngine) {
  const auto path = MakeTestPath();
  std::shared_ptr<const sitos::StorageReader> snapshot;
  std::shared_ptr<sitos::rocksdb_test::EventLog> events;
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    events = sitos::rocksdb_test::GetEventLog(*engine);
    auto cleanup = std::make_shared<CleanupToken>(path, events);
    ASSERT_TRUE(engine->Put("stable", std::vector<std::byte>{std::byte{0x01}}));
    snapshot = std::make_shared<SnapshotAdapter>(engine->TakeSnapshot(), cleanup);
    ASSERT_NE(snapshot, nullptr);
    engine.reset();
    EXPECT_TRUE(snapshot->Get("stable", [](std::string_view, sitos::Bytes value) {
      return value.size() == 1 && value[0] == std::byte{0x01};
    }));
  }
  ASSERT_EQ(events->events.size(), 1u);
  EXPECT_EQ(events->events[0], "get_snapshot");
  snapshot.reset();
  ASSERT_EQ(events->events.size(), 4u);
  EXPECT_EQ(events->events[0], "get_snapshot");
  EXPECT_EQ(events->events[1], "release_snapshot");
  EXPECT_EQ(events->events[2], "db_close");
  EXPECT_EQ(events->events[3], "directory_remove");
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(RocksDBEngineConcurrency, ConcurrentPutDeleteGetList) {
  const auto path = MakeTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_TRUE(result.IsOk());
  auto engine = std::move(result).Value();
  ASSERT_NE(engine, nullptr);
  auto snapshot = engine->TakeSnapshot();
  ASSERT_NE(snapshot, nullptr);

  constexpr int kWriters = 2;
  constexpr int kReaders = 2;
  constexpr int kOperations = 128;
  constexpr int kThreads = kWriters + kReaders;
  std::barrier start(kThreads + 1);
  std::atomic<int> errors{0};
  std::atomic<int> completed{0};

  auto writer = [&](int id) {
    start.arrive_and_wait();
    for (int i = 0; i < kOperations; ++i) {
      const auto key = "concurrent/" + std::to_string(id) + "/" + std::to_string(i);
      const std::vector<std::byte> value{
          std::byte{static_cast<unsigned char>(id)},
          std::byte{static_cast<unsigned char>(i)}};
      if (!engine->Put(key, value)) ++errors;
      if ((i % 2) == 0 && !engine->Delete(key)) ++errors;
    }
    const auto final_key = "final/" + std::to_string(id);
    const std::vector<std::byte> final_value{
        std::byte{static_cast<unsigned char>(id)}};
    if (!engine->Put(final_key, final_value)) ++errors;
    ++completed;
  };

  auto reader = [&] {
    start.arrive_and_wait();
    for (int i = 0; i < kOperations; ++i) {
      const auto key = "concurrent/" + std::to_string(i % kWriters) + "/" +
                       std::to_string(i);
      engine->Get(key, [&](std::string_view, sitos::Bytes value) {
        if (value.size() != 2) ++errors;
        return true;
      });
      if (!engine->List("concurrent/", [&](std::string_view entry_key, sitos::Bytes value) {
            if (!entry_key.starts_with("concurrent/") || value.size() != 2) ++errors;
            return true;
          })) {
        ++errors;
      }
    }
    ++completed;
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int id = 0; id < kWriters; ++id) threads.emplace_back(writer, id);
  for (int i = 0; i < kReaders; ++i) threads.emplace_back(reader);
  start.arrive_and_wait();
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(completed.load(), kThreads);
  EXPECT_EQ(errors.load(), 0);
  for (int id = 0; id < kWriters; ++id) {
    const auto final_key = "final/" + std::to_string(id);
    bool called = false;
    ASSERT_TRUE(engine->Get(final_key, [&](std::string_view, sitos::Bytes value) {
      called = true;
      EXPECT_EQ(value.size(), 1u);
      if (value.size() == 1) {
        EXPECT_EQ(value[0], std::byte{static_cast<unsigned char>(id)});
      }
      return true;
    }));
    EXPECT_TRUE(called);
  }
  std::size_t final_count = 0;
  ASSERT_TRUE(engine->List("final/", [&](std::string_view, sitos::Bytes value) {
    ++final_count;
    return value.size() == 1;
  }));
  EXPECT_EQ(final_count, static_cast<std::size_t>(kWriters));

  snapshot.reset();
  engine.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  EXPECT_FALSE(error);
}

TEST(RocksDBEngineNativeFailure, PutDeleteGetListHaveContractedFailures) {
  const auto path = MakeTestPath();
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

TEST(RocksDBEngineSnapshotLifetime, MultipleSnapshotsRemainIndependent) {
  const auto path = MakeTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  ASSERT_TRUE(result.IsOk());
  auto engine = std::move(result).Value();
  ASSERT_TRUE(engine->Put("key", std::vector<std::byte>{std::byte{0x01}}));
  auto first = engine->TakeSnapshot();
  ASSERT_TRUE(engine->Put("key", std::vector<std::byte>{std::byte{0x02}}));
  auto second = engine->TakeSnapshot();
  ASSERT_TRUE(engine->Put("key", std::vector<std::byte>{std::byte{0x03}}));
  ASSERT_TRUE(first->Get("key", [](std::string_view, sitos::Bytes value) {
    return value.size() == 1 && value[0] == std::byte{0x01};
  }));
  ASSERT_TRUE(second->Get("key", [](std::string_view, sitos::Bytes value) {
    return value.size() == 1 && value[0] == std::byte{0x02};
  }));
  std::error_code error;
  engine.reset();
  first.reset();
  second.reset();
  std::filesystem::remove_all(path, error);
}
