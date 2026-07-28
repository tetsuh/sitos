// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/rocksdb_engine.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SITOS_WITH_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#endif

namespace sitos {
namespace {

#if defined(SITOS_WITH_ROCKSDB)
using RocksDBOpenResult = decltype(rocksdb::DB::Open(
    std::declval<const rocksdb::Options&>(), std::declval<const std::string&>(),
    std::declval<std::unique_ptr<rocksdb::DB>*>()));
static_assert(std::is_same_v<RocksDBOpenResult, rocksdb::Status>);

class RocksDBErrorCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "sitos.rocksdb"; }
  std::string message(int value) const override {
    return "RocksDB failure (code " + std::to_string(value) + ")";
  }
};

const std::error_category& RocksDBCategory() noexcept {
  static const RocksDBErrorCategory category;
  return category;
}

std::error_code MakeRocksDBError(const rocksdb::Status& status) {
  return {static_cast<int>(status.code()) + 1, RocksDBCategory()};
}
#endif

constexpr unsigned int kFailPut = 1U << 0;
constexpr unsigned int kFailDelete = 1U << 1;
constexpr unsigned int kFailGet = 1U << 2;
constexpr unsigned int kFailList = 1U << 3;

#if defined(SITOS_WITH_ROCKSDB)
struct DatabaseState {
  explicit DatabaseState(std::shared_ptr<rocksdb::DB> database) : db(std::move(database)) {}

  std::shared_ptr<rocksdb::DB> db;
  std::atomic<unsigned int> failures{0};
  std::atomic<std::size_t> snapshot_calls{0};
  std::atomic<std::size_t> enumeration_calls{0};
};

bool Enumerate(rocksdb::DB& db, const rocksdb::ReadOptions& options,
               std::string_view prefix, const EntrySink& sink,
               std::atomic<std::size_t>* enumeration_calls = nullptr) {
  if (enumeration_calls != nullptr) {
    enumeration_calls->fetch_add(1, std::memory_order_relaxed);
  }
  std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
  std::unique_ptr<rocksdb::Iterator> iterator(db.NewIterator(options));
  for (iterator->Seek(rocksdb::Slice(prefix.data(), prefix.size())); iterator->Valid();
       iterator->Next()) {
    const rocksdb::Slice key = iterator->key();
    if (!std::string_view(key.data(), key.size()).starts_with(prefix)) break;
    const rocksdb::Slice value = iterator->value();
    entries.emplace_back(std::string(key.data(), key.size()),
                         std::vector<std::byte>(
                             reinterpret_cast<const std::byte*>(value.data()),
                             reinterpret_cast<const std::byte*>(value.data()) + value.size()));
  }
  if (!iterator->status().ok()) return false;
  for (const auto& [entry_key, value] : entries) {
    if (!sink(entry_key, value)) return false;
  }
  return true;
}

class RocksDBSnapshot final : public StorageReader {
 public:
  RocksDBSnapshot(std::shared_ptr<DatabaseState> state, const rocksdb::Snapshot* snapshot)
      : state_(std::move(state)), snapshot_(snapshot) {}

  ~RocksDBSnapshot() override {
    if (snapshot_ != nullptr) state_->db->ReleaseSnapshot(snapshot_);
  }

  bool Get(std::string_view key, const EntrySink& sink) const override {
    if ((state_->failures.load(std::memory_order_relaxed) & kFailGet) != 0) return false;
    rocksdb::ReadOptions options;
    options.snapshot = snapshot_;
    std::string value;
    const rocksdb::Status status =
        state_->db->Get(options, rocksdb::Slice(key.data(), key.size()), &value);
    if (status.IsNotFound() || !status.ok()) return false;
    sink(key, std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                          value.size()));
    return true;
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    if ((state_->failures.load(std::memory_order_relaxed) & kFailList) != 0) return false;
    rocksdb::ReadOptions options;
    options.snapshot = snapshot_;
    return Enumerate(*state_->db, options, prefix, sink, &state_->enumeration_calls);
  }

 private:
  std::shared_ptr<DatabaseState> state_;
  const rocksdb::Snapshot* snapshot_;
};
#endif

}  // namespace

struct RocksDBEngine::Impl {
#if defined(SITOS_WITH_ROCKSDB)
  explicit Impl(std::shared_ptr<DatabaseState> database) : state(std::move(database)) {}
  std::shared_ptr<DatabaseState> state;
#endif
};

RocksDBEngine::RocksDBEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RocksDBEngine::~RocksDBEngine() = default;

Result<std::unique_ptr<RocksDBEngine>> RocksDBEngine::Open(const std::string& path) {
#if defined(SITOS_WITH_ROCKSDB)
  if (path.empty()) {
    return Result<std::unique_ptr<RocksDBEngine>>::Err(
        Status::InvalidArgument, "RocksDB path must not be empty",
        std::make_error_code(std::errc::invalid_argument));
  }
  rocksdb::Options options;
  options.create_if_missing = true;
  std::unique_ptr<rocksdb::DB> owned_db;
  const rocksdb::Status status = rocksdb::DB::Open(options, path, &owned_db);
  if (!status.ok()) {
    return Result<std::unique_ptr<RocksDBEngine>>::Err(Status::Error,
                                                       "RocksDB open failed: " + status.ToString(),
                                                       MakeRocksDBError(status));
  }
  std::shared_ptr<rocksdb::DB> shared_db(std::move(owned_db));
  auto database = std::make_shared<DatabaseState>(std::move(shared_db));
  return Result<std::unique_ptr<RocksDBEngine>>::Ok(
      std::unique_ptr<RocksDBEngine>(
          new RocksDBEngine(std::make_unique<Impl>(std::move(database)))));
#else
  static_cast<void>(path);
  return Result<std::unique_ptr<RocksDBEngine>>::Err(
      Status::Error, "RocksDB support is disabled",
      std::make_error_code(std::errc::operation_not_supported));
#endif
}

bool RocksDBEngine::Put(std::string_view key, Bytes value) {
#if defined(SITOS_WITH_ROCKSDB)
  if ((impl_->state->failures.load(std::memory_order_relaxed) & kFailPut) != 0) return false;
  const rocksdb::Status status = impl_->state->db->Put(
      rocksdb::WriteOptions{}, rocksdb::Slice(key.data(), key.size()),
      rocksdb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
  return status.ok();
#else
  static_cast<void>(key);
  static_cast<void>(value);
  return false;
#endif
}

bool RocksDBEngine::Delete(std::string_view key) {
#if defined(SITOS_WITH_ROCKSDB)
  if ((impl_->state->failures.load(std::memory_order_relaxed) & kFailDelete) != 0) return false;
  return impl_->state->db
      ->Delete(rocksdb::WriteOptions{}, rocksdb::Slice(key.data(), key.size()))
      .ok();
#else
  static_cast<void>(key);
  return false;
#endif
}

bool RocksDBEngine::Get(std::string_view key, const EntrySink& sink) const {
#if defined(SITOS_WITH_ROCKSDB)
  if ((impl_->state->failures.load(std::memory_order_relaxed) & kFailGet) != 0) return false;
  std::string value;
  const rocksdb::Status status = impl_->state->db->Get(
      rocksdb::ReadOptions{}, rocksdb::Slice(key.data(), key.size()), &value);
  if (status.IsNotFound() || !status.ok()) return false;
  sink(key, std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                        value.size()));
  return true;
#else
  static_cast<void>(key);
  static_cast<void>(sink);
  return false;
#endif
}

bool RocksDBEngine::List(std::string_view prefix, const EntrySink& sink) const {
#if defined(SITOS_WITH_ROCKSDB)
  if ((impl_->state->failures.load(std::memory_order_relaxed) & kFailList) != 0) return false;
  rocksdb::ReadOptions options;
  return Enumerate(*impl_->state->db, options, prefix, sink,
                   &impl_->state->enumeration_calls);
#else
  static_cast<void>(prefix);
  static_cast<void>(sink);
  return false;
#endif
}

std::shared_ptr<const StorageReader> RocksDBEngine::TakeSnapshot() const {
#if defined(SITOS_WITH_ROCKSDB)
  impl_->state->snapshot_calls.fetch_add(1, std::memory_order_relaxed);
  const rocksdb::Snapshot* snapshot = impl_->state->db->GetSnapshot();
  try {
    return std::make_shared<RocksDBSnapshot>(impl_->state, snapshot);
  } catch (...) {
    impl_->state->db->ReleaseSnapshot(snapshot);
    throw;
  }
#else
  return nullptr;
#endif
}

void RocksDBEngine::GetSnapshotStatsForTest(std::size_t& snapshot_calls,
                                             std::size_t& enumeration_calls) const {
#if defined(SITOS_WITH_ROCKSDB)
  snapshot_calls = impl_->state->snapshot_calls.load(std::memory_order_relaxed);
  enumeration_calls = impl_->state->enumeration_calls.load(std::memory_order_relaxed);
#else
  snapshot_calls = 0;
  enumeration_calls = 0;
#endif
}

void RocksDBEngine::SetFailureMaskForTest(unsigned int mask) {
#if defined(SITOS_WITH_ROCKSDB)
  impl_->state->failures.store(mask, std::memory_order_relaxed);
#else
  static_cast<void>(mask);
#endif
}

}  // namespace sitos
