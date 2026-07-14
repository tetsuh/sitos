// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <cctype>
#include <sitos/key.hpp>

namespace sitos {
namespace {

bool IsValidChunkChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

bool IsValidChunk(std::string_view chunk) {
  if (chunk.empty()) {
    return false;
  }
  for (char c : chunk) {
    if (!IsValidChunkChar(c)) {
      return false;
    }
  }
  return true;
}

// Validates a slash-separated sequence of one or more chunks.
bool IsValidChunkSequence(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '/' || value.back() == '/') {
    return false;
  }
  std::size_t start = 0;
  while (start <= value.size()) {
    std::size_t end = value.find('/', start);
    if (end == std::string_view::npos) {
      end = value.size();
    }
    if (!IsValidChunk(value.substr(start, end - start))) {
      return false;
    }
    if (end == value.size()) {
      break;
    }
    start = end + 1;
  }
  return true;
}

// The special batch segment, always the last chunk of a batch key.
constexpr std::string_view kBatchSegment = ":batch";

bool IsValidIdChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

bool IsValidId(std::string_view id) {
  if (id.empty()) {
    return false;
  }
  for (char c : id) {
    if (!IsValidIdChar(c)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool IsValidKey(std::string_view key) { return IsValidChunkSequence(key); }

bool IsValidSessionId(std::string_view sid) { return IsValidId(sid); }

bool IsValidPrefix(std::string_view prefix) { return IsValidChunkSequence(prefix); }

bool IsValidAckUuid(std::string_view uuid) {
  // Lenient: accept any non-empty [0-9a-zA-Z_-] sequence. The canonical UUID
  // form (8-4-4-4-12 hex) is a subset; we stay strict about zenoh-reserved
  // characters and reject '/' so meta/ack/<uuid> cannot smuggle extra chunks.
  return IsValidId(uuid);
}

std::optional<Scope> ParseScope(std::string_view scope) {
  if (scope == "base") {
    return Scope{ScopeKind::Base, ""};
  }
  constexpr std::string_view kSessionPrefix = "session/";
  constexpr std::string_view kSnapPrefix = "snap/";
  if (scope.size() > kSessionPrefix.size() &&
      scope.starts_with(kSessionPrefix)) {
    std::string_view sid = scope.substr(kSessionPrefix.size());
    if (IsValidSessionId(sid)) {
      return Scope{ScopeKind::Session, std::string(sid)};
    }
  }
  if (scope.size() > kSnapPrefix.size() && scope.starts_with(kSnapPrefix)) {
    std::string_view sid = scope.substr(kSnapPrefix.size());
    if (IsValidSessionId(sid)) {
      return Scope{ScopeKind::Snap, std::string(sid)};
    }
  }
  return std::nullopt;
}

std::optional<std::string> BuildKey(std::string_view prefix, std::string_view scope,
                                    std::string_view user_key) {
  if (!IsValidPrefix(prefix) || !ParseScope(scope).has_value() || !IsValidKey(user_key)) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(prefix.size() + 1 + scope.size() + 1 + user_key.size());
  result.append(prefix);
  result.push_back('/');
  result.append(scope);
  result.push_back('/');
  result.append(user_key);
  return result;
}

std::optional<std::string> BuildBatchKey(std::string_view prefix, std::string_view scope) {
  if (!IsValidPrefix(prefix)) {
    return std::nullopt;
  }
  // Batch is defined for base and session scopes only (docs/03 §1.1).
  if (scope == "base") {
    std::string result;
    result.reserve(prefix.size() + 1 + scope.size() + 1 + kBatchSegment.size());
    result.append(prefix);
    result.push_back('/');
    result.append(scope);
    result.push_back('/');
    result.append(kBatchSegment);
    return result;
  }
  constexpr std::string_view kSessionPrefix = "session/";
  if (scope.size() > kSessionPrefix.size() &&
      scope.starts_with(kSessionPrefix)) {
    std::string_view sid = scope.substr(kSessionPrefix.size());
    if (!IsValidSessionId(sid)) {
      return std::nullopt;
    }
    std::string result;
    result.reserve(prefix.size() + 1 + scope.size() + 1 + kBatchSegment.size());
    result.append(prefix);
    result.push_back('/');
    result.append(scope);
    result.push_back('/');
    result.append(kBatchSegment);
    return result;
  }
  // snap (and any other) has no batch path.
  return std::nullopt;
}

std::optional<std::string> BuildMetaSessionKey(std::string_view prefix, std::string_view sid) {
  if (!IsValidPrefix(prefix) || !IsValidSessionId(sid)) {
    return std::nullopt;
  }
  constexpr std::string_view kMeta = "meta/session";
  std::string result;
  result.reserve(prefix.size() + 1 + kMeta.size() + 1 + sid.size());
  result.append(prefix);
  result.push_back('/');
  result.append(kMeta);
  result.push_back('/');
  result.append(sid);
  return result;
}

std::optional<std::string> BuildMetaAckKey(std::string_view prefix, std::string_view uuid) {
  if (!IsValidPrefix(prefix) || !IsValidAckUuid(uuid)) {
    return std::nullopt;
  }
  constexpr std::string_view kMeta = "meta/ack";
  std::string result;
  result.reserve(prefix.size() + 1 + kMeta.size() + 1 + uuid.size());
  result.append(prefix);
  result.push_back('/');
  result.append(kMeta);
  result.push_back('/');
  result.append(uuid);
  return result;
}

namespace {

// Returns the remainder of `full_key` after `<prefix>/`, or std::nullopt if
// `full_key` does not start with `prefix` followed by '/'.
std::optional<std::string_view> StripPrefix(std::string_view prefix, std::string_view full_key) {
  if (full_key.size() <= prefix.size() || full_key[prefix.size()] != '/') {
    return std::nullopt;
  }
  if (!full_key.starts_with(prefix)) {
    return std::nullopt;
  }
  return full_key.substr(prefix.size() + 1);
}

// Splits `rest` at the first '/' into (head, tail). Returns std::nullopt if
// there is no '/' (tail would be empty).
std::optional<std::pair<std::string_view, std::string_view>> SplitFirst(std::string_view rest) {
  std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{rest.substr(0, slash), rest.substr(slash + 1)};
}

}  // namespace

std::optional<ParsedKey> ParseKey(std::string_view prefix, std::string_view full_key) {
  auto rest_opt = StripPrefix(prefix, full_key);
  if (!rest_opt) {
    return std::nullopt;
  }
  std::string_view rest = *rest_opt;

  // base/<key...> and base/:batch
  if (rest == "base") {
    // <prefix>/base with no user key is not a valid value key.
    return std::nullopt;
  }
  if (auto split = SplitFirst(rest)) {
    auto [head, tail] = *split;
    if (head == "base") {
      if (tail == kBatchSegment) {
        return ParsedKey{KeyKind::Base, "", "", "", true};
      }
      if (!IsValidKey(tail)) {
        return std::nullopt;
      }
      return ParsedKey{KeyKind::Base, "", "", std::string(tail), false};
    }
    if (head == "session") {
      // session/<sid>/<key...> or session/<sid>/:batch
      auto sid_split = SplitFirst(tail);
      if (!sid_split) {
        return std::nullopt;  // session/ with no sid/key
      }
      auto [sid, value] = *sid_split;
      if (!IsValidSessionId(sid)) {
        return std::nullopt;
      }
      if (value == kBatchSegment) {
        return ParsedKey{KeyKind::Session, std::string(sid), "", "", true};
      }
      if (!IsValidKey(value)) {
        return std::nullopt;
      }
      return ParsedKey{KeyKind::Session, std::string(sid), "", std::string(value), false};
    }
    if (head == "snap") {
      // snap/<sid>/<key...> — read-only, no :batch.
      auto sid_split = SplitFirst(tail);
      if (!sid_split) {
        return std::nullopt;
      }
      auto [sid, value] = *sid_split;
      if (!IsValidSessionId(sid)) {
        return std::nullopt;
      }
      if (!IsValidKey(value)) {
        return std::nullopt;  // rejects snap/<sid>/:batch as a read-only batch path
      }
      return ParsedKey{KeyKind::Snapshot, std::string(sid), "", std::string(value), false};
    }
    if (head == "meta") {
      // meta/session/<sid> or meta/ack/<uuid>
      auto meta_split = SplitFirst(tail);
      if (!meta_split) {
        return std::nullopt;
      }
      auto [meta_kind, id] = *meta_split;
      if (meta_kind == "session") {
        if (!IsValidSessionId(id)) {
          return std::nullopt;
        }
        return ParsedKey{KeyKind::MetaSession, std::string(id), "", "", false};
      }
      if (meta_kind == "ack") {
        if (!IsValidAckUuid(id)) {
          return std::nullopt;
        }
        return ParsedKey{KeyKind::MetaAck, "", std::string(id), "", false};
      }
    }
  }
  return std::nullopt;
}

}  // namespace sitos