// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Batch codec for the sitos.v1.batch payload.
// See docs/03_wire_protocol.md §5.

#ifndef SITOS_BATCH_HPP
#define SITOS_BATCH_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sitos/param_value.hpp"

namespace sitos {

/// One entry in a decoded batch.
struct BatchEntry {
  std::string key;
  ParamValue value;
};

/// Encode a batch (sitos.v1.batch layout, docs/03 §5). Keys are written as the
/// relative UTF-8 key; values reuse the payload v1 body (without type tag).
[[nodiscard]] std::vector<std::byte> EncodeBatch(
    std::span<const std::pair<std::string, ParamValue>> entries);

/// Encode a batch from the canonical BatchEntry representation.
[[nodiscard]] std::vector<std::byte> EncodeBatch(std::span<const BatchEntry> entries);

/// Decode a batch payload. Returns std::nullopt on malformed input
/// (short count, truncated key/value, unknown value tag, or trailing bytes).
[[nodiscard]] std::optional<std::vector<BatchEntry>> DecodeBatch(
    std::span<const std::byte> payload);

}  // namespace sitos

#endif  // SITOS_BATCH_HPP