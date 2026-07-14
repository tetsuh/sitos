// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// User-key grammar validation, scope parsing, key-expression building, and
// incoming key parsing for the sitos key space.
// See docs/03_wire_protocol.md §1 and docs/02_architecture.md §2.

#ifndef SITOS_KEY_HPP
#define SITOS_KEY_HPP

#include <optional>
#include <string>
#include <string_view>

namespace sitos {

/// Scope classification used by ParamStore/StorageNode APIs.
enum class ScopeKind { Base, Session, Snap };

/// Parsed representation of a scope string.
struct Scope {
  ScopeKind kind;
  std::string sid;
};

/// Key kind, used by the incoming-key parser for StorageNode routing.
/// See docs/03_wire_protocol.md §1.1.
enum class KeyKind {
  Base,         ///< <prefix>/base/<key> (or <prefix>/base/:batch)
  Session,      ///< <prefix>/session/<sid>/<key> (or :batch)
  Snapshot,     ///< <prefix>/snap/<sid>/<key> (read-only; no :batch)
  MetaSession,  ///< <prefix>/meta/session/<sid>
  MetaAck,      ///< <prefix>/meta/ack/<uuid>
};

/// Result of parsing an incoming zenoh key expression. The inverse of the
/// Build*Key functions. See docs/03_wire_protocol.md §1.1.
struct ParsedKey {
  KeyKind kind;
  /// Session id for Session / Snapshot / MetaSession. Empty for Base / MetaAck.
  std::string sid;
  /// Ack UUID for MetaAck. Empty for all other kinds.
  std::string uuid;
  /// Relative user key for Base / Session / Snapshot. Empty for Meta paths and
  /// for :batch paths (where is_batch is true instead).
  std::string relative_key;
  /// True for the special <prefix>/base/:batch and <prefix>/session/<sid>/:batch
  /// paths. Only Base and Session may be batch.
  bool is_batch = false;
};

/// Validates a user key according to wire protocol §1.2. Rejects empty
/// chunks, leading/trailing '/', whitespace, and every character outside
/// [0-9A-Za-z_.-], including the ':' batch-control marker.
bool IsValidKey(std::string_view key);

/// Validates a session id according to wire protocol §1.1 ([0-9a-zA-Z_-]+,
/// single chunk).
bool IsValidSessionId(std::string_view sid);

/// Validates a key prefix (one or more chunks).
bool IsValidPrefix(std::string_view prefix);

/// Validates an ack UUID. Accepts the canonical 8-4-4-4-12 hex form and any
/// non-empty sequence of [0-9a-zA-Z_-] (a safe superset); rejects zenoh-reserved
/// characters.
bool IsValidAckUuid(std::string_view uuid);

/// Parses scope strings "base", "session/<sid>", or "snap/<sid>".
std::optional<Scope> ParseScope(std::string_view scope);

/// Builds a full zenoh key for a user value: <prefix>/<scope>/<key>.
/// Returns std::nullopt if any component is invalid.
std::optional<std::string> BuildKey(std::string_view prefix, std::string_view scope,
                                    std::string_view user_key);

/// Builds a :batch key for the given scope: <prefix>/base/:batch or
/// <prefix>/session/<sid>/:batch. Batch is not defined for snap; returns
/// std::nullopt for a snap scope.
std::optional<std::string> BuildBatchKey(std::string_view prefix, std::string_view scope);

/// Builds the session-metadata key <prefix>/meta/session/<sid>.
std::optional<std::string> BuildMetaSessionKey(std::string_view prefix, std::string_view sid);

/// Builds the ack key <prefix>/meta/ack/<uuid>.
std::optional<std::string> BuildMetaAckKey(std::string_view prefix, std::string_view uuid);

/// Parses an incoming zenoh key expression that is expected to live under the
/// given prefix. Strips the prefix and classifies the remainder according to
/// docs/03 §1.1. Returns std::nullopt if the prefix does not match or the
/// remainder is not a valid sitos key.
///
/// Precondition: `prefix` must be a valid prefix (IsValidPrefix == true).
/// Callers validate the prefix once at setup (e.g. StorageNode::Start) rather
/// than on every message, so ParseKey does not re-validate it.
std::optional<ParsedKey> ParseKey(std::string_view prefix, std::string_view full_key);

}  // namespace sitos

#endif  // SITOS_KEY_HPP