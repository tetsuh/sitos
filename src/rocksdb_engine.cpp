// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/rocksdb_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SITOS_WITH_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#endif

#if defined(SITOS_WITH_ROCKSDB)
#include "rocksdb_engine_test_access.hpp"
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
    return std::format("RocksDB failure (code {})", value);
  }
};

const std::error_category& RocksDBCategory() noexcept {
  static const RocksDBErrorCategory category;
  return category;
}

std::error_code MakeRocksDBError(const rocksdb::Status& status) {
  using StatusCodeType = std::underlying_type_t<rocksdb::Status::Code>;
  const auto code = static_cast<StatusCodeType>(status.code());
  return {static_cast<int>(code) + 1, RocksDBCategory()};
}

struct OperationAdapter {
  using Put = std::function<rocksdb::Status(rocksdb::DB&, const rocksdb::WriteOptions&,
                                             const rocksdb::Slice&, const rocksdb::Slice&)>;
  using Delete = std::function<rocksdb::Status(rocksdb::DB&, const rocksdb::WriteOptions&,
                                                const rocksdb::Slice&)>;
  using Get = std::function<rocksdb::Status(rocksdb::DB&, const rocksdb::ReadOptions&,
                                             const rocksdb::Slice&, std::string*)>;
  using List = std::function<rocksdb::Status(
      rocksdb::DB&, const rocksdb::ReadOptions&, std::string_view,
      std::vector<std::pair<std::string, std::vector<std::byte>>>&)>;

  Put put;
  Delete delete_key;
  Get get;
  List list;
};

OperationAdapter MakeOperationAdapter(unsigned int failures = 0) {
  OperationAdapter adapter;
  adapter.put = [failures](rocksdb::DB& db, const rocksdb::WriteOptions& options,
                           const rocksdb::Slice& key, const rocksdb::Slice& value) {
    if ((failures & rocksdb_test::kPut) != 0) {
      return rocksdb::Status::IOError("injected Put failure");
    }
    return db.Put(options, key, value);
  };
  adapter.delete_key = [failures](rocksdb::DB& db, const rocksdb::WriteOptions& options,
                                  const rocksdb::Slice& key) {
    if ((failures & rocksdb_test::kDelete) != 0) {
      return rocksdb::Status::IOError("injected Delete failure");
    }
    return db.Delete(options, key);
  };
  adapter.get = [failures](rocksdb::DB& db, const rocksdb::ReadOptions& options,
                           const rocksdb::Slice& key, std::string* value) {
    if ((failures & rocksdb_test::kGet) != 0) {
      return rocksdb::Status::IOError("injected Get failure");
    }
    return db.Get(options, key, value);
  };
  adapter.list = [failures](rocksdb::DB& db, const rocksdb::ReadOptions& options,
                            std::string_view prefix,
                            std::vector<std::pair<std::string, std::vector<std::byte>>>& entries) {
    if ((failures & rocksdb_test::kList) != 0) {
      return rocksdb::Status::IOError("injected List failure");
    }
    std::unique_ptr<rocksdb::Iterator> iterator(db.NewIterator(options));
    iterator->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
    while (iterator->Valid()) {
      const rocksdb::Slice key = iterator->key();
      if (!std::string_view(key.data(), key.size()).starts_with(prefix)) break;
      const rocksdb::Slice value = iterator->value();
      entries.emplace_back(
          std::string(key.data(), key.size()),
          std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                reinterpret_cast<const std::byte*>(value.data()) + value.size()));
      iterator->Next();
    }
    return iterator->status();
  };
  return adapter;
}

struct DatabaseState {
  DatabaseState(std::shared_ptr<rocksdb::DB> database,
                std::shared_ptr<rocksdb_test::EventLog> event_log)
      : db(std::move(database)), events(std::move(event_log)), operations(MakeOperationAdapter()) {}

  std::shared_ptr<rocksdb::DB> db;
  std::shared_ptr<rocksdb_test::EventLog> events;
  std::mutex operations_mutex;
  OperationAdapter operations;
  std::atomic<std::size_t> snapshot_calls{0};
  std::atomic<std::size_t> enumeration_calls{0};
};

OperationAdapter Operations(const std::shared_ptr<DatabaseState>& state) {
  std::lock_guard lock(state->operations_mutex);
  return state->operations;
}

class RocksDBSnapshot final : public StorageReader {
 public:
  RocksDBSnapshot(std::shared_ptr<DatabaseState> state, const rocksdb::Snapshot* snapshot)
      : state_(std::move(state)), snapshot_(snapshot) {}

  ~RocksDBSnapshot() override {
    if (snapshot_ != nullptr) {
      state_->db->ReleaseSnapshot(snapshot_);
      state_->events->Add("release_snapshot");
    }
  }

  RocksDBSnapshot(const RocksDBSnapshot&) = delete;
  RocksDBSnapshot& operator=(const RocksDBSnapshot&) = delete;
  RocksDBSnapshot(RocksDBSnapshot&&) = delete;
  RocksDBSnapshot& operator=(RocksDBSnapshot&&) = delete;

  bool Get(std::string_view key, const EntrySink& sink) const override {
    std::string value;
    rocksdb::ReadOptions options;
    options.snapshot = snapshot_;
    if (const rocksdb::Status status = Operations(state_).get(
            *state_->db, options, rocksdb::Slice(key.data(), key.size()), &value);
        status.IsNotFound() || !status.ok()) {
      return false;
    }
    sink(key, std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                          value.size()));
    return true;
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    rocksdb::ReadOptions options;
    options.snapshot = snapshot_;
    state_->enumeration_calls.fetch_add(1);
    const rocksdb::Status status = Operations(state_).list(*state_->db, options, prefix, entries);
    if (!status.ok()) return false;
    return std::ranges::all_of(entries, [&sink](const auto& entry) {
      const auto& [entry_key, value] = entry;
      return sink(entry_key, value);
    });
  }

 private:
  std::shared_ptr<DatabaseState> state_;
  const rocksdb::Snapshot* snapshot_;
};

std::mutex registry_mutex;
std::unordered_map<const RocksDBEngine*, std::weak_ptr<DatabaseState>> registry;

void Register(const RocksDBEngine* engine, const std::shared_ptr<DatabaseState>& state) {
  std::lock_guard lock(registry_mutex);
  registry[engine] = state;
}

void Unregister(const RocksDBEngine* engine) {
  std::lock_guard lock(registry_mutex);
  registry.erase(engine);
}

std::shared_ptr<DatabaseState> StateFor(const RocksDBEngine* engine) {
  std::lock_guard lock(registry_mutex);
  const auto it = registry.find(engine);
  return it == registry.end() ? nullptr : it->second.lock();
}
#endif

}  // namespace

struct RocksDBEngine::Impl {
#if defined(SITOS_WITH_ROCKSDB)
  explicit Impl(std::shared_ptr<DatabaseState> database) : state(std::move(database)) {}
  std::shared_ptr<DatabaseState> state;
#endif
};

RocksDBEngine::RocksDBEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
#if defined(SITOS_WITH_ROCKSDB)
  Register(this, impl_->state);
#endif
}

RocksDBEngine::~RocksDBEngine() {
#if defined(SITOS_WITH_ROCKSDB)
  Unregister(this);
#endif
}

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
  if (const rocksdb::Status status = rocksdb::DB::Open(options, path, &owned_db);
      !status.ok()) {
    return Result<std::unique_ptr<RocksDBEngine>>::Err(
        Status::Error, std::format("RocksDB open failed: {}", status.ToString()),
        MakeRocksDBError(status));
  }
  auto event_log = std::make_shared<rocksdb_test::EventLog>();
  std::weak_ptr<rocksdb_test::EventLog> weak_events = event_log;
  std::shared_ptr<rocksdb::DB> shared_db(
      owned_db.release(), [weak_events](rocksdb::DB* database) {
        delete database;
        if (const auto events = weak_events.lock()) events->Add("db_close");
      });
  auto database = std::make_shared<DatabaseState>(std::move(shared_db), std::move(event_log));
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
  const rocksdb::Status status = Operations(impl_->state).put(
      *impl_->state->db, rocksdb::WriteOptions{}, rocksdb::Slice(key.data(), key.size()),
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
  return Operations(impl_->state).delete_key(
             *impl_->state->db, rocksdb::WriteOptions{}, rocksdb::Slice(key.data(), key.size()))
      .ok();
#else
  static_cast<void>(key);
  return false;
#endif
}

bool RocksDBEngine::Get(std::string_view key, const EntrySink& sink) const {
#if defined(SITOS_WITH_ROCKSDB)
  std::string value;
  if (const rocksdb::Status status = Operations(impl_->state).get(
          *impl_->state->db, rocksdb::ReadOptions{}, rocksdb::Slice(key.data(), key.size()),
          &value);
      status.IsNotFound() || !status.ok()) {
    return false;
  }
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
  std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
  impl_->state->enumeration_calls.fetch_add(1);
  if (const rocksdb::Status status = Operations(impl_->state).list(
          *impl_->state->db, rocksdb::ReadOptions{}, prefix, entries);
      !status.ok()) {
    return false;
  }
  return std::ranges::all_of(entries, [&sink](const auto& entry) {
    const auto& [entry_key, value] = entry;
    return sink(entry_key, value);
  });
#else
  static_cast<void>(prefix);
  static_cast<void>(sink);
  return false;
#endif
}

std::shared_ptr<const StorageReader> RocksDBEngine::TakeSnapshot() const {
#if defined(SITOS_WITH_ROCKSDB)
  impl_->state->snapshot_calls.fetch_add(1);
  const rocksdb::Snapshot* snapshot = impl_->state->db->GetSnapshot();
  impl_->state->events->Add("get_snapshot");
  try {
    return std::make_shared<RocksDBSnapshot>(impl_->state, snapshot);
  } catch (...) {
    impl_->state->db->ReleaseSnapshot(snapshot);
    impl_->state->events->Add("release_snapshot");
    throw;
  }
#else
  return nullptr;
#endif
}

#if defined(SITOS_WITH_ROCKSDB)
namespace rocksdb_test {

void SetFailures(RocksDBEngine& engine, unsigned int failures) {
  const auto state = StateFor(&engine);
  if (state == nullptr) return;
  std::lock_guard lock(state->operations_mutex);
  state->operations = MakeOperationAdapter(failures);
}

void GetSnapshotStats(const RocksDBEngine& engine, std::size_t& snapshot_calls,
                      std::size_t& enumeration_calls) {
  const auto state = StateFor(&engine);
  if (state == nullptr) {
    snapshot_calls = 0;
    enumeration_calls = 0;
    return;
  }
  snapshot_calls = state->snapshot_calls.load();
  enumeration_calls = state->enumeration_calls.load();
}

std::shared_ptr<rocksdb_test::EventLog> GetEventLog(const RocksDBEngine& engine) {
  const auto state = StateFor(&engine);
  return state == nullptr ? nullptr : state->events;
}

}  // namespace rocksdb_test
#endif

}  // namespace sitos
