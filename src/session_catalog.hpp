// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Durable session catalog (ADR-0036 §D1-§D4, §D9). One exclusive RocksDB
// instance under <durable_root>/catalog records each durable-buffer Session's
// lifecycle and generation. Every write is synchronized before it returns.

#ifndef SITOS_SESSION_CATALOG_HPP
#define SITOS_SESSION_CATALOG_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sitos/result.hpp"
#include "sitos/session.hpp"

namespace sitos::catalog_internal {

/// Sanitized diagnostics for an incomplete deletion: no paths or payloads.
struct SessionCatalogFailure {
  std::string operation;
  std::string category;
  int code = 0;

  bool operator==(const SessionCatalogFailure&) const = default;
};

/// One versioned catalog record. Empty timestamps are absent (encoded as null).
struct SessionCatalogRecord {
  std::string sid;
  std::string generation_uuid;
  SessionLifecycleState state = SessionLifecycleState::kActive;
  std::string owner_instance_id;
  bool durable = true;
  bool ephemeral = false;
  std::string created_at;
  std::string retained_at;
  std::string orphaned_at;
  std::string deleting_at;
  std::string deleted_at;
  std::optional<SessionCatalogFailure> failure;

  bool operator==(const SessionCatalogRecord&) const = default;
};

/// Returns the wire and catalog spelling, for example "delete_failed".
std::string_view LifecycleName(SessionLifecycleState state) noexcept;

/// Encodes the canonical schema-1 JSON text of a record.
std::string EncodeSessionRecord(const SessionCatalogRecord& record);

/// Decodes a canonical schema-1 record stored under `sid`; nullopt when the text
/// is not canonical, names another sid, or violates a field rule.
std::optional<SessionCatalogRecord> DecodeSessionRecord(std::string_view sid,
                                                        std::string_view text);

class SessionCatalogTestAccess;

class SessionCatalog {
 public:
  /// Opens <durable_root>/catalog, creating it only for an empty root, validates
  /// every entry, and records `instance_id`. Every failure is CatalogUnavailable.
  static Result<std::unique_ptr<SessionCatalog>> Open(const std::filesystem::path& durable_root,
                                                      std::string_view instance_id,
                                                      std::string_view started_at);

  ~SessionCatalog();
  SessionCatalog(const SessionCatalog&) = delete;
  SessionCatalog& operator=(const SessionCatalog&) = delete;

  std::optional<SessionCatalogRecord> Find(std::string_view sid) const;
  std::vector<SessionCatalogRecord> Records() const;

  /// Writes one record with a synchronized write. On failure the previous durable
  /// record stays authoritative and the in-memory view is unchanged.
  Result<void> Put(const SessionCatalogRecord& record);

 private:
  friend class SessionCatalogTestAccess;
  struct Impl;
  explicit SessionCatalog(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Raw catalog mutation for tests that simulate corruption and offline repair.
class SessionCatalogTestAccess {
 public:
  static Result<void> PutRaw(SessionCatalog& catalog, std::string_view key, std::string_view value);
  static Result<void> DeleteRaw(SessionCatalog& catalog, std::string_view key);
  static Result<void> PutRawOffline(const std::filesystem::path& durable_root, std::string_view key,
                                    std::string_view value);
  static Result<void> DeleteRawOffline(const std::filesystem::path& durable_root,
                                       std::string_view key);
};

}  // namespace sitos::catalog_internal

#endif  // SITOS_SESSION_CATALOG_HPP
