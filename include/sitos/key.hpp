// Copyright 2026 Tetsuya Hayashi
// SPDX-License-Identifier: Apache-2.0

#pragma once

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

/// Validates a user key according to wire protocol §1.2.
bool IsValidKey(std::string_view key);

/// Validates a session id according to wire protocol §1.1.
bool IsValidSessionId(std::string_view sid);

/// Validates a key prefix (one or more chunks).
bool IsValidPrefix(std::string_view prefix);

/// Parses scope strings "base", "session/<sid>", or "snap/<sid>".
std::optional<Scope> ParseScope(std::string_view scope);

/// Builds a full zenoh key: <prefix>/<scope>/<user_key>.
std::optional<std::string> BuildKey(std::string_view prefix,
                                    std::string_view scope,
                                    std::string_view user_key);

}  // namespace sitos
