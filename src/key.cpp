// Copyright 2026 Tetsuya Hayashi
// SPDX-License-Identifier: Apache-2.0

#include <sitos/key.hpp>

#include <cctype>

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

}  // namespace

bool IsValidKey(std::string_view key) { return IsValidChunkSequence(key); }

bool IsValidSessionId(std::string_view sid) {
  if (sid.empty()) {
    return false;
  }
  for (char c : sid) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool IsValidPrefix(std::string_view prefix) { return IsValidChunkSequence(prefix); }

std::optional<Scope> ParseScope(std::string_view scope) {
  if (scope == "base") {
    return Scope{ScopeKind::Base, ""};
  }
  constexpr std::string_view kSessionPrefix = "session/";
  constexpr std::string_view kSnapPrefix = "snap/";
  if (scope.size() > kSessionPrefix.size() &&
      scope.substr(0, kSessionPrefix.size()) == kSessionPrefix) {
    std::string_view sid = scope.substr(kSessionPrefix.size());
    if (IsValidSessionId(sid)) {
      return Scope{ScopeKind::Session, std::string(sid)};
    }
  }
  if (scope.size() > kSnapPrefix.size() && scope.substr(0, kSnapPrefix.size()) == kSnapPrefix) {
    std::string_view sid = scope.substr(kSnapPrefix.size());
    if (IsValidSessionId(sid)) {
      return Scope{ScopeKind::Snap, std::string(sid)};
    }
  }
  return std::nullopt;
}

std::optional<std::string> BuildKey(std::string_view prefix,
                                    std::string_view scope,
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

}  // namespace sitos
