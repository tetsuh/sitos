// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_LIST_PREFIX_VALIDATION_HPP
#define SITOS_LIST_PREFIX_VALIDATION_HPP

#include <cctype>
#include <string>
#include <string_view>

#include "sitos/key.hpp"
#include "sitos/result.hpp"

namespace sitos::param_detail {

inline bool ContainsWhitespaceOrWildcard(std::string_view value) {
  for (char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isspace(byte) != 0 || c == '*' || c == '?') return true;
  }
  return false;
}

inline bool ContainsBatchSegment(std::string_view value) {
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find('/', start);
    const std::size_t length = end == std::string_view::npos ? value.size() - start : end - start;
    if (value.substr(start, length) == ":batch") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

inline Result<void> ValidateListPrefix(std::string_view prefix) {
  if (prefix.empty()) return Result<void>::Ok();
  if (prefix.front() == '/' || prefix.find("//") != std::string_view::npos ||
      ContainsWhitespaceOrWildcard(prefix) || ContainsBatchSegment(prefix)) {
    return Result<void>::Err(Status::InvalidKey, "invalid list prefix");
  }
  if (prefix.back() == '/') {
    if (prefix.size() == 1 || !IsValidPrefix(prefix.substr(0, prefix.size() - 1))) {
      return Result<void>::Err(Status::InvalidKey, "invalid list prefix");
    }
    return Result<void>::Ok();
  }
  if (!IsValidKey(prefix)) return Result<void>::Err(Status::InvalidKey, "invalid list prefix");
  return Result<void>::Ok();
}

}  // namespace sitos::param_detail

#endif  // SITOS_LIST_PREFIX_VALIDATION_HPP
