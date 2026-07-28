// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "storage_engine_contract.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sitos/rocksdb_engine.hpp"
#include "rocksdb_engine_test_access.hpp"

namespace {

std::filesystem::path MakeTestPath() {
  static std::atomic<unsigned int> next_id{0};
  const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() /
         ("sitos-rocksdb-test-" + std::to_string(id));
}

class ContractEngine final : public sitos::StorageEngine {
 public:
  ContractEngine(std::unique_ptr<sitos::RocksDBEngine> engine, std::filesystem::path path)
      : engine_(std::move(engine)), path_(std::move(path)) {}

  ~ContractEngine() override {
    engine_.reset();
    std::error_code error;
    std::filesystem::remove_all(path_, error);
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
    return engine_->TakeSnapshot();
  }

 private:
  std::unique_ptr<sitos::RocksDBEngine> engine_;
  std::filesystem::path path_;
};

std::unique_ptr<sitos::StorageEngine> MakeContractEngine() {
  const auto path = MakeTestPath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  if (!result.IsOk()) return nullptr;
  return std::make_unique<ContractEngine>(std::move(result).Value(), path);
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
  sitos::RocksDBEngineTestAccess::GetSnapshotStats(*engine, snapshot_calls, enumeration_calls);
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
  {
    auto result = sitos::RocksDBEngine::Open(path.string());
    ASSERT_TRUE(result.IsOk());
    auto engine = std::move(result).Value();
    ASSERT_TRUE(engine->Put("stable", std::vector<std::byte>{std::byte{0x01}}));
    snapshot = engine->TakeSnapshot();
    ASSERT_NE(snapshot, nullptr);
    engine.reset();
    EXPECT_TRUE(snapshot->Get("stable", [](std::string_view, sitos::Bytes value) {
      return value.size() == 1 && value[0] == std::byte{0x01};
    }));
  }
  snapshot.reset();
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

  sitos::RocksDBEngineTestAccess::SetFailures(*engine, sitos::RocksDBEngineTestAccess::kPut);
  EXPECT_FALSE(engine->Put("fault", std::vector<std::byte>{std::byte{0x02}}));
  EXPECT_TRUE(engine->Get("fault", [](std::string_view, sitos::Bytes value) {
    return value.size() == 1 && value[0] == std::byte{0x01};
  }));
  sitos::RocksDBEngineTestAccess::SetFailures(*engine, sitos::RocksDBEngineTestAccess::kDelete);
  EXPECT_FALSE(engine->Delete("fault"));

  bool called = false;
  sitos::RocksDBEngineTestAccess::SetFailures(*engine, sitos::RocksDBEngineTestAccess::kGet);
  EXPECT_FALSE(engine->Get("fault", [&](std::string_view, sitos::Bytes) {
    called = true;
    return true;
  }));
  EXPECT_FALSE(called);
  sitos::RocksDBEngineTestAccess::SetFailures(*engine, sitos::RocksDBEngineTestAccess::kList);
  EXPECT_FALSE(engine->List("", [&](std::string_view, sitos::Bytes) {
    called = true;
    return true;
  }));
  EXPECT_FALSE(called);

  sitos::RocksDBEngineTestAccess::SetFailures(*engine, 0);
  std::error_code error;
  std::filesystem::remove_all(path, error);
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
    return value[0] == std::byte{0x01};
  }));
  ASSERT_TRUE(second->Get("key", [](std::string_view, sitos::Bytes value) {
    return value[0] == std::byte{0x02};
  }));
  std::error_code error;
  engine.reset();
  first.reset();
  second.reset();
  std::filesystem::remove_all(path, error);
}
