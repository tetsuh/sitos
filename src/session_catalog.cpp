// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "session_catalog.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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
  return std::ranges::none_of(text, [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u >= 0x7f || c == '"' || c == '\\';
  });
}

// Reads one flat canonical object. Each Take* call either consumes its token
// and succeeds, or fails without a partial result.
class FlatJsonReader {
 public:
  explicit FlatJsonReader(std::string_view text) : text_(text) {}

  std::optional<std::vector<JsonMember>> ReadObject() {
    std::vector<JsonMember> members;
    if (!Take('{')) return std::nullopt;
    bool more = !Take('}');
    while (more) {
      auto name = TakeString();
      if (!name || !Take(':')) return std::nullopt;
      auto value = TakeValue();
      if (!value) return std::nullopt;
      members.emplace_back(std::move(*name), std::move(*value));
      more = !Take('}');
      if (more && !Take(',')) return std::nullopt;
    }
    if (at_ != text_.size()) return std::nullopt;
    return members;
  }

 private:
  bool Take(char expected) {
    if (at_ >= text_.size() || text_[at_] != expected) return false;
    ++at_;
    return true;
  }

  bool TakeWord(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }

  std::optional<std::string> TakeString() {
    if (!Take('"')) return std::nullopt;
    const auto end = text_.find('"', at_);
    if (end == std::string_view::npos) return std::nullopt;
    std::string value(text_.substr(at_, end - at_));
    if (!IsPlainText(value)) return std::nullopt;
    at_ = end + 1;
    return value;
  }

  // A canonical integer: optional minus sign, no leading zero, fits int64.
  std::optional<std::int64_t> TakeInteger() {
    const auto begin = at_;
    if (at_ < text_.size() && text_[at_] == '-') ++at_;
    const auto first_digit = at_;
    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
    if (const auto digit_count = at_ - first_digit;
        digit_count == 0 || (digit_count > 1 && text_[first_digit] == '0')) {
      return std::nullopt;
    }
    const auto digits = text_.substr(begin, at_ - begin);
    std::int64_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    return number;
  }

  std::optional<JsonValue> TakeValue() {
    if (at_ < text_.size() && text_[at_] == '"') {
      auto text = TakeString();
      if (!text) return std::nullopt;
      return JsonValue{std::move(*text)};
    }
    if (TakeWord("null")) return JsonValue{std::monostate{}};
    if (TakeWord("true")) return JsonValue{true};
    if (TakeWord("false")) return JsonValue{false};
    if (auto number = TakeInteger(); number.has_value()) return JsonValue{*number};
    return std::nullopt;
  }

  std::string_view text_;
  std::size_t at_ = 0;
};

std::optional<std::vector<JsonMember>> ParseFlatObject(std::string_view text) {
  return FlatJsonReader(text).ReadObject();
}

std::string Quote(std::string_view text) { return std::format("\"{}\"", text); }

std::string NullableText(const std::string& text) {
  return text.empty() ? std::string("null") : Quote(text);
}

bool IsCanonicalUuid(std::string_view text) {
  const auto parsed = fence_internal::ParseFenceUuid(text);
  return parsed.has_value() && fence_internal::FormatFenceUuid(*parsed) == text;
}

// Reads `width` decimal digits at `at` as a number within [low, high].
bool DigitsInRange(std::string_view text, std::size_t at, std::size_t width, int low, int high) {
  if (at + width > text.size()) return false;
  int value = 0;
  for (std::size_t i = at; i < at + width; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
  }
  return value >= low && value <= high;
}

// ISO-8601 UTC: YYYY-MM-DDTHH:MM:SS, an optional '.' and 1-9 fraction digits, then 'Z'.
bool IsTimestamp(std::string_view text) {
  constexpr std::size_t kSeconds = 19;  // length of "YYYY-MM-DDTHH:MM:SS"
  if (text.size() < kSeconds + 1 || text.back() != 'Z') return false;
  const bool fields =
      DigitsInRange(text, 0, 4, 0, 9999) && text[4] == '-' && DigitsInRange(text, 5, 2, 1, 12) &&
      text[7] == '-' && DigitsInRange(text, 8, 2, 1, 31) && text[10] == 'T' &&
      DigitsInRange(text, 11, 2, 0, 23) && text[13] == ':' && DigitsInRange(text, 14, 2, 0, 59) &&
      text[16] == ':' && DigitsInRange(text, 17, 2, 0, 60);
  if (!fields) return false;
  // The day must exist in that month of that year, leap days included.
  const auto number = [&text](std::size_t at, std::size_t width) {
    int value = 0;
    for (std::size_t i = at; i < at + width; ++i) value = value * 10 + (text[i] - '0');
    return value;
  };
  const std::chrono::year_month_day date{std::chrono::year{number(0, 4)},
                                         std::chrono::month{static_cast<unsigned>(number(5, 2))},
                                         std::chrono::day{static_cast<unsigned>(number(8, 2))}};
  if (!date.ok()) return false;
  const auto fraction = text.substr(kSeconds, text.size() - kSeconds - 1);
  if (fraction.empty()) return true;
  return fraction.size() >= 2 && fraction.size() <= 10 && fraction.front() == '.' &&
         std::ranges::all_of(fraction.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}

bool IsDiagnosticWord(std::string_view text) {
  return !text.empty() && text.size() <= 32 &&
         std::ranges::all_of(text, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

std::optional<SessionLifecycleState> ParseLifecycle(std::string_view text) {
  for (const auto state : {SessionLifecycleState::kActive, SessionLifecycleState::kRetained,
                           SessionLifecycleState::kOrphaned, SessionLifecycleState::kDeleting,
                           SessionLifecycleState::kDeleteFailed, SessionLifecycleState::kDeleted}) {
    if (LifecycleName(state) == text) return state;
  }
  return std::nullopt;
}

constexpr std::array<std::string_view, 15> kRecordMembers = {
    "schema_version",    "sid",
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

// ADR-0036 §D3/§D4: each lifecycle state carries the timestamps of the transitions that
// reached it, and only a pending or failed deletion may carry failure diagnostics.
bool HasLifecycleMetadata(const SessionCatalogRecord& record) {
  const bool retained = !record.retained_at.empty();
  const bool orphaned = !record.orphaned_at.empty();
  const bool deleting = !record.deleting_at.empty();
  const bool deleted = !record.deleted_at.empty();
  const bool failure = record.failure.has_value();
  switch (record.state) {
    case SessionLifecycleState::kActive:
      return !retained && !orphaned && !deleting && !deleted && !failure;
    case SessionLifecycleState::kRetained:
      return retained && !deleting && !deleted && !failure;
    case SessionLifecycleState::kOrphaned:
      return orphaned && !deleting && !deleted && !failure;
    case SessionLifecycleState::kDeleting:
      return deleting && !deleted;
    case SessionLifecycleState::kDeleteFailed:
      return deleting && !deleted && failure;
    case SessionLifecycleState::kDeleted:
      return deleting && deleted && !failure;
  }
  return false;
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
  const std::array<std::string*, 4> timestamps = {&record.retained_at, &record.orphaned_at,
                                                  &record.deleting_at, &record.deleted_at};
  for (std::size_t i = 0; i < std::size(timestamps); ++i) {
    auto value = TextOrNull(m[8 + i].value, ok);
    if (!ok || !value || (!value->empty() && !IsTimestamp(*value))) return std::nullopt;
    *timestamps[i] = std::move(*value);
  }
  if (const bool failure_absent = std::holds_alternative<std::monostate>(m[12].value) &&
                                  std::holds_alternative<std::monostate>(m[13].value) &&
                                  std::holds_alternative<std::monostate>(m[14].value);
      !failure_absent) {
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
  if (!HasLifecycleMetadata(record)) return std::nullopt;
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
  if (const auto status = rocksdb::DB::Open(options, directory.string(), &db); !status.ok()) {
    return Result<std::unique_ptr<rocksdb::DB>>::Err(
        Status::CatalogUnavailable, std::format("catalog open failed: {}", status.ToString()));
  }
  return Result<std::unique_ptr<rocksdb::DB>>::Ok(std::move(db));
}

// Validates every catalog entry and loads the session records (ADR-0036 §D3).
Result<void> LoadEntries(rocksdb::DB& db,
                         std::map<std::string, SessionCatalogRecord, std::less<>>& records) {
  const auto invalid = [](std::string message) {
    return Result<void>::Err(Status::CatalogUnavailable, std::move(message));
  };
  bool schema_seen = false;
  std::unique_ptr<rocksdb::Iterator> iterator(db.NewIterator(rocksdb::ReadOptions()));
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    const std::string_view key(iterator->key().data(), iterator->key().size());
    const std::string_view value(iterator->value().data(), iterator->value().size());
    if (key == kSchemaKey) {
      if (value != kSchemaValue) return invalid("catalog schema is not supported");
      schema_seen = true;
    } else if (key == kInstanceKey) {
      if (!ValidInstanceValue(value)) return invalid("catalog instance entry is invalid");
    } else if (key.starts_with(kSessionKeyPrefix)) {
      const auto sid = key.substr(kSessionKeyPrefix.size());
      auto record = DecodeSessionRecord(sid, value);
      if (!record) return invalid("catalog session record is invalid");
      records.try_emplace(std::string(sid), std::move(*record));
    } else {
      return invalid("catalog contains an unknown entry");
    }
  }
  if (!iterator->status().ok()) return invalid("catalog read failed");
  if (!schema_seen) return invalid("catalog schema entry is missing");
  return Result<void>::Ok();
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

  if (auto loaded = LoadEntries(*impl->db, impl->records); !loaded.IsOk()) {
    return Result<std::unique_ptr<SessionCatalog>>::ErrFrom(loaded);
  }
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
