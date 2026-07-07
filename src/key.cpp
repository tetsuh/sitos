// Copyright 2026 Tetsuya Hayashi
// SPDX-License-Identifier: Apache-2.0

#include <sitos/key.hpp>

namespace sitos {

bool IsValidKey(std::string_view /*key*/) { return false; }

bool IsValidSessionId(std::string_view /*sid*/) { return false; }

bool IsValidPrefix(std::string_view /*prefix*/) { return false; }

std::optional<Scope> ParseScope(std::string_view /*scope*/) { return std::nullopt; }

std::optional<std::string> BuildKey(std::string_view /*prefix*/,
                                    std::string_view /*scope*/,
                                    std::string_view /*user_key*/) {
  return std::nullopt;
}

}  // namespace sitos
