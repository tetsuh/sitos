// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "session_catalog.hpp"

#include <charconv>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>
#include <variant>

#include "fence_internal.hpp"
#include "sitos/key.hpp"

#if defined(SITOS_WITH_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>
#endif

namespace sitos::catalog_internal {
namespace {

constexpr std::string_view kSchemaKey = "schema";
constexpr std::string_view kInstanceKey = "instance";
constexpr std::string_view kSessionKeyPrefix = "session/";
constexpr std::string_view kSchemaValue = R"({"schema_version":1})";

// ---------------------------------------------------------------------------
// Flat canonical JSON: one object of string, integer, boolean, or null members,
// no whitespace, no escapes. Callers then check the exact member order.
// ---------------------------------------------------------------------------

using JsonValue = std::variant<std::monostate, std::string, std::int64_t, bool>;

struct JsonMember {
  std::string name;
  JsonValue value;
};

bool IsPlainText(std::string_view text) noexcept {
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u >= 0x7f || c == '"' || c == '\\') return false;
  }
  return true;
}

std::optional<std::vector<JsonMember>> ParseFlatObject(std::string_view text) {
  std::size_t at = 0;
  auto take = [&](char expected) {
    if (at >= text.size() || text[at] != expected) return false;
    ++at;
    return true;
  };
  auto take_word = [&](std::string_view word) {
    if (text.substr(at, word.size()) != word) return false;
    at += word.size();
    return true;
  };
  auto take_string = [&]() -> std::optional<std::string> {
    if (!take('"')) return std::nullopt;
    const auto end = text.find('"', at);
    if (end == std::string_view::npos) return std::nullopt;
    std::string value(text.substr(at, end - at));
    if (!IsPlainText(value)) return std::nullopt;
    at = end + 1;
    return value;
  };

  std::vector<JsonMember> members;
  if (!take('{')) return std::nullopt;
  if (take('}')) return at == text.size() ? std::optional(members) : std::nullopt;
  for (;;) {
    auto name = take_string();
    if (!name || !take(':')) return std::nullopt;
    JsonValue value;
    if (at < text.size() && text[at] == '"') {
      auto string_value = take_string();
      if (!string_value) return std::nullopt;
      value = std::move(*string_value);
    } else if (take_word("null")) {
      value = std::monostate{};
    } else if (take_word("true")) {
      value = true;
    } else if (take_word("false")) {
      value = false;
    } else {
      const auto begin = at;
      if (at < text.size() && text[at] == '-') ++at;
      while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
      const auto digits = text.substr(begin, at - begin);
      const auto unsigned_digits = digits.starts_with('-') ? digits.substr(1) : digits;
      if (unsigned_digits.empty() ||
          (unsigned_digits.size() > 1 && unsigned_digits.front() == '0')) {
        return std::nullopt;
      }
      std::int64_t number = 0;
      const auto [end, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), number);
      if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
      value = number;
    }
    members.push_back({std::move(*name), std::move(value)});
    if (take('}')) break;
    if (!take(',')) return std::nullopt;
  }
  if (at != text.size()) return std::nullopt;
  return members;
}

std::string Quote(std::string_view text) { return std::format("\"{}\"", text); }

std::string NullableText(const std::string& text) {
  return text.empty() ? std::string("null") : Quote(text);
}

bool IsCanonicalUuid(std::string_view text) {
  const auto parsed = fence_internal::ParseFenceUuid(text);
  return parsed.has_value() && fence_internal::FormatFenceUuid(*parsed) == text;
}

bool IsTimestamp(std::string_view text) {
  if (text.empty() || text.size() > 64) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || c == 'T' || c == 'Z' || c == ':' || c == '-' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool IsDiagnosticWord(std::string_view text) {
  if (text.empty() || text.size() > 32) return false;
  for (const char c : text) {
    if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
  }
  return true;
}

std::optional<SessionLifecycleState> ParseLifecycle(std::string_view text) {
  for (const auto state : {SessionLifecycleState::kActive, SessionLifecycleState::kRetained,
                           SessionLifecycleState::kOrphaned, SessionLifecycleState::kDeleting,
                           SessionLifecycleState::kDeleteFailed, SessionLifecycleState::kDeleted}) {
    if (LifecycleName(state) == text) return state;
  }
  return std::nullopt;
}

constexpr std::string_view kRecordMembers[] = {"schema_version",    "sid",
                                               "generation_uuid",   "state",
                                               "owner_instance_id", "durable",
                                               "ephemeral",         "created_at",
                                               "retained_at",       "orphaned_at",
                                               "deleting_at",       "deleted_at",
                                               "failure_operation", "failure_category",
                                               "failure_code"};

std::optional<std::string> TextOrNull(const JsonValue& value, bool& ok) {
  if (std::holds_alternative<std::monostate>(value)) return std::string();
  if (const auto* text = std::get_if<std::string>(&value)) return *text;
  ok = false;
  return std::nullopt;
}

}  // namespace

std::string_view LifecycleName(SessionLifecycleState state) noexcept {
  switch (state) {
    case SessionLifecycleState::kActive:
      return "active";
    case SessionLifecycleState::kRetained:
      return "retained";
    case SessionLifecycleState::kOrphaned:
      return "orphaned";
    case SessionLifecycleState::kDeleting:
      return "deleting";
    case SessionLifecycleState::kDeleteFailed:
      return "delete_failed";
    case SessionLifecycleState::kDeleted:
      return "deleted";
  }
  return "unknown";
}

std::string EncodeSessionRecord(const SessionCatalogRecord& record) {
  const auto& failure = record.failure;
  return std::format(
      R"({{"schema_version":1,"sid":{},"generation_uuid":{},"state":{},"owner_instance_id":{},)"
      R"("durable":{},"ephemeral":{},"created_at":{},"retained_at":{},"orphaned_at":{},)"
      R"("deleting_at":{},"deleted_at":{},"failure_operation":{},"failure_category":{},)"
      R"("failure_code":{}}})",
      Quote(record.sid), Quote(record.generation_uuid), Quote(LifecycleName(record.state)),
      Quote(record.owner_instance_id), record.durable ? "true" : "false",
      record.ephemeral ? "true" : "false", Quote(record.created_at),
      NullableText(record.retained_at), NullableText(record.orphaned_at),
      NullableText(record.deleting_at), NullableText(record.deleted_at),
      failure ? Quote(failure->operation) : std::string("null"),
      failure ? Quote(failure->category) : std::string("null"),
      failure ? std::to_string(failure->code) : std::string("null"));
}

std::optional<SessionCatalogRecord> DecodeSessionRecord(std::string_view sid,
                                                        std::string_view text) {
  const auto members = ParseFlatObject(text);
  if (!members || members->size() != std::size(kRecordMembers)) return std::nullopt;
  for (std::size_t i = 0; i < members->size(); ++i) {
    if ((*members)[i].name != kRecordMembers[i]) return std::nullopt;
  }
  const auto& m = *members;
  const auto* schema = std::get_if<std::int64_t>(&m[0].value);
  const auto* record_sid = std::get_if<std::string>(&m[1].value);
  const auto* generation = std::get_if<std::string>(&m[2].value);
  const auto* state_text = std::get_if<std::string>(&m[3].value);
  const auto* owner = std::get_if<std::string>(&m[4].value);
  const auto* durable = std::get_if<bool>(&m[5].value);
  const auto* ephemeral = std::get_if<bool>(&m[6].value);
  const auto* created = std::get_if<std::string>(&m[7].value);
  if (schema == nullptr || *schema != 1 || record_sid == nullptr || *record_sid != sid ||
      !IsValidSessionId(*record_sid) || generation == nullptr || !IsCanonicalUuid(*generation) ||
      state_text == nullptr || owner == nullptr || !IsCanonicalUuid(*owner) || durable == nullptr ||
      !*durable || ephemeral == nullptr || created == nullptr || !IsTimestamp(*created)) {
    return std::nullopt;
  }
  const auto state = ParseLifecycle(*state_text);
  if (!state) return std::nullopt;

  SessionCatalogRecord record;
  record.sid = *record_sid;
  record.generation_uuid = *generation;
  record.state = *state;
  record.owner_instance_id = *owner;
  record.durable = *durable;
  record.ephemeral = *ephemeral;
  record.created_at = *created;
  bool ok = true;
  std::string* timestamps[] = {&record.retained_at, &record.orphaned_at, &record.deleting_at,
                               &record.deleted_at};
  for (std::size_t i = 0; i < std::size(timestamps); ++i) {
    auto value = TextOrNull(m[8 + i].value, ok);
    if (!ok || !value || (!value->empty() && !IsTimestamp(*value))) return std::nullopt;
    *timestamps[i] = std::move(*value);
  }
  const bool failure_absent = std::holds_alternative<std::monostate>(m[12].value) &&
                              std::holds_alternative<std::monostate>(m[13].value) &&
                              std::holds_alternative<std::monostate>(m[14].value);
  if (!failure_absent) {
    const auto* operation = std::get_if<std::string>(&m[12].value);
    const auto* category = std::get_if<std::string>(&m[13].value);
    const auto* code = std::get_if<std::int64_t>(&m[14].value);
    if (operation == nullptr || category == nullptr || code == nullptr ||
        !IsDiagnosticWord(*operation) || !IsDiagnosticWord(*category) || *code < INT32_MIN ||
        *code > INT32_MAX) {
      return std::nullopt;
    }
    record.failure = SessionCatalogFailure{*operation, *category, static_cast<int>(*code)};
  }
  return record;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

struct SessionCatalog::Impl {
#if defined(SITOS_WITH_ROCKSDB)
  std::unique_ptr<rocksdb::DB> db;
#endif
  mutable std::mutex mutex;
  std::map<std::string, SessionCatalogRecord, std::less<>> records;
};

SessionCatalog::SessionCatalog(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SessionCatalog::~SessionCatalog() = default;

std::optional<SessionCatalogRecord> SessionCatalog::Find(std::string_view sid) const {
  std::scoped_lock lock(impl_->mutex);
  const auto it = impl_->records.find(sid);
  if (it == impl_->records.end()) return std::nullopt;
  return it->second;
}

std::vector<SessionCatalogRecord> SessionCatalog::Records() const {
  std::scoped_lock lock(impl_->mutex);
  std::vector<SessionCatalogRecord> records;
  records.reserve(impl_->records.size());
  for (const auto& [sid, record] : impl_->records) records.push_back(record);
  return records;
}

#if defined(SITOS_WITH_ROCKSDB)
namespace {

bool ValidInstanceValue(std::string_view text) {
  const auto members = ParseFlatObject(text);
  if (!members || members->size() != 2 || (*members)[0].name != "instance_id" ||
      (*members)[1].name != "started_at") {
    return false;
  }
  const auto* id = std::get_if<std::string>(&(*members)[0].value);
  const auto* started = std::get_if<std::string>(&(*members)[1].value);
  return id != nullptr && started != nullptr && IsCanonicalUuid(*id) && IsTimestamp(*started);
}

std::string InstanceValue(std::string_view instance_id, std::string_view started_at) {
  return std::format(R"({{"instance_id":"{}","started_at":"{}"}})", instance_id, started_at);
}

Result<std::unique_ptr<SessionCatalog>> Unavailable(std::string message) {
  return Result<std::unique_ptr<SessionCatalog>>::Err(Status::CatalogUnavailable,
                                                      std::move(message));
}

rocksdb::WriteOptions SyncedWrite() {
  rocksdb::WriteOptions options;
  options.sync = true;
  options.disableWAL = false;
  return options;
}

Result<std::unique_ptr<rocksdb::DB>> OpenRaw(const std::filesystem::path& directory, bool create) {
  rocksdb::Options options;
  options.create_if_missing = create;
  options.error_if_exists = create;
  std::unique_ptr<rocksdb::DB> db;
  const auto status = rocksdb::DB::Open(options, directory.string(), &db);
  if (!status.ok()) {
    return Result<std::unique_ptr<rocksdb::DB>>::Err(
        Status::CatalogUnavailable, std::format("catalog open failed: {}", status.ToString()));
  }
  return Result<std::unique_ptr<rocksdb::DB>>::Ok(std::move(db));
}

bool IsEmptyOrAbsentDirectory(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return !error;
  if (!std::filesystem::is_directory(path, error) || error) return false;
  return std::filesystem::is_empty(path, error) && !error;
}

}  // namespace

Result<std::unique_ptr<SessionCatalog>> SessionCatalog::Open(
    const std::filesystem::path& durable_root, std::string_view instance_id,
    std::string_view started_at) {
  const auto catalog_directory = durable_root / "catalog";
  std::error_code error;
  const bool exists = std::filesystem::exists(catalog_directory, error);
  if (error) return Unavailable("catalog directory is not accessible");

  auto impl = std::make_unique<Impl>();
  if (!exists) {
    // First use is only an empty root: a root holding Session stores without a
    // catalog lost its catalog and must not be silently re-created (ADR-0036 §D3).
    if (!IsEmptyOrAbsentDirectory(durable_root / "sessions")) {
      return Unavailable("catalog is missing beside existing session stores");
    }
    std::filesystem::create_directories(durable_root, error);
    if (error) return Unavailable("durable root cannot be created");
    auto opened = OpenRaw(catalog_directory, /*create=*/true);
    if (!opened.IsOk()) return Result<std::unique_ptr<SessionCatalog>>::ErrFrom(opened);
    impl->db = std::move(opened).Value();
    rocksdb::WriteBatch batch;
    batch.Put(kSchemaKey, kSchemaValue);
    batch.Put(kInstanceKey, InstanceValue(instance_id, started_at));
    if (!impl->db->Write(SyncedWrite(), &batch).ok()) {
      return Unavailable("catalog initialization write failed");
    }
    return Result<std::unique_ptr<SessionCatalog>>::Ok(
        std::unique_ptr<SessionCatalog>(new SessionCatalog(std::move(impl))));
  }

  auto opened = OpenRaw(catalog_directory, /*create=*/false);
  if (!opened.IsOk()) return Result<std::unique_ptr<SessionCatalog>>::ErrFrom(opened);
  impl->db = std::move(opened).Value();

  bool schema_seen = false;
  {
    std::unique_ptr<rocksdb::Iterator> iterator(impl->db->NewIterator(rocksdb::ReadOptions()));
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      const std::string_view key(iterator->key().data(), iterator->key().size());
      const std::string_view value(iterator->value().data(), iterator->value().size());
      if (key == kSchemaKey) {
        if (value != kSchemaValue) return Unavailable("catalog schema is not supported");
        schema_seen = true;
      } else if (key == kInstanceKey) {
        if (!ValidInstanceValue(value)) return Unavailable("catalog instance entry is invalid");
      } else if (key.starts_with(kSessionKeyPrefix)) {
        const auto sid = key.substr(kSessionKeyPrefix.size());
        auto record = DecodeSessionRecord(sid, value);
        if (!record) return Unavailable("catalog session record is invalid");
        impl->records.emplace(std::string(sid), std::move(*record));
      } else {
        return Unavailable("catalog contains an unknown entry");
      }
    }
    if (!iterator->status().ok()) return Unavailable("catalog read failed");
  }
  if (!schema_seen) return Unavailable("catalog schema entry is missing");
  if (!impl->db->Put(SyncedWrite(), kInstanceKey, InstanceValue(instance_id, started_at)).ok()) {
    return Unavailable("catalog instance write failed");
  }
  return Result<std::unique_ptr<SessionCatalog>>::Ok(
      std::unique_ptr<SessionCatalog>(new SessionCatalog(std::move(impl))));
}

Result<void> SessionCatalog::Put(const SessionCatalogRecord& record) {
  const std::string key = std::string(kSessionKeyPrefix) + record.sid;
  const std::string value = EncodeSessionRecord(record);
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->db->Put(SyncedWrite(), key, value).ok()) {
    return Result<void>::Err(Status::CatalogUnavailable, "catalog write failed");
  }
  impl_->records.insert_or_assign(record.sid, record);
  return Result<void>::Ok();
}

Result<void> SessionCatalogTestAccess::PutRaw(SessionCatalog& catalog, std::string_view key,
                                              std::string_view value) {
  std::scoped_lock lock(catalog.impl_->mutex);
  if (!catalog.impl_->db->Put(SyncedWrite(), key, value).ok()) {
    return Result<void>::Err(Status::Error, "raw catalog write failed");
  }
  return Result<void>::Ok();
}

Result<void> SessionCatalogTestAccess::DeleteRaw(SessionCatalog& catalog, std::string_view key) {
  std::scoped_lock lock(catalog.impl_->mutex);
  if (!catalog.impl_->db->Delete(SyncedWrite(), key).ok()) {
    return Result<void>::Err(Status::Error, "raw catalog delete failed");
  }
  return Result<void>::Ok();
}

Result<void> SessionCatalogTestAccess::PutRawOffline(const std::filesystem::path& durable_root,
                                                     std::string_view key, std::string_view value) {
  auto opened = OpenRaw(durable_root / "catalog", /*create=*/false);
  if (!opened.IsOk()) return Result<void>::ErrFrom(opened);
  if (!opened.Value()->Put(SyncedWrite(), key, value).ok()) {
    return Result<void>::Err(Status::Error, "raw catalog write failed");
  }
  return Result<void>::Ok();
}

Result<void> SessionCatalogTestAccess::DeleteRawOffline(const std::filesystem::path& durable_root,
                                                        std::string_view key) {
  auto opened = OpenRaw(durable_root / "catalog", /*create=*/false);
  if (!opened.IsOk()) return Result<void>::ErrFrom(opened);
  if (!opened.Value()->Delete(SyncedWrite(), key).ok()) {
    return Result<void>::Err(Status::Error, "raw catalog delete failed");
  }
  return Result<void>::Ok();
}

#else  // SITOS_WITH_ROCKSDB

Result<std::unique_ptr<SessionCatalog>> SessionCatalog::Open(const std::filesystem::path&,
                                                             std::string_view, std::string_view) {
  return Result<std::unique_ptr<SessionCatalog>>::Err(
      Status::InvalidArgument, "the durable session catalog requires RocksDB support",
      std::make_error_code(std::errc::operation_not_supported));
}

Result<void> SessionCatalog::Put(const SessionCatalogRecord&) {
  return Result<void>::Err(Status::CatalogUnavailable, "catalog support is disabled");
}

Result<void> SessionCatalogTestAccess::PutRaw(SessionCatalog&, std::string_view, std::string_view) {
  return Result<void>::Err(Status::Error, "catalog support is disabled");
}

Result<void> SessionCatalogTestAccess::DeleteRaw(SessionCatalog&, std::string_view) {
  return Result<void>::Err(Status::Error, "catalog support is disabled");
}

Result<void> SessionCatalogTestAccess::PutRawOffline(const std::filesystem::path&, std::string_view,
                                                     std::string_view) {
  return Result<void>::Err(Status::Error, "catalog support is disabled");
}

Result<void> SessionCatalogTestAccess::DeleteRawOffline(const std::filesystem::path&,
                                                        std::string_view) {
  return Result<void>::Err(Status::Error, "catalog support is disabled");
}

#endif  // SITOS_WITH_ROCKSDB

}  // namespace sitos::catalog_internal
