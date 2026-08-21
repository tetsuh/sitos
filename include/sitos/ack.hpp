// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Acknowledgement substrate shared by acknowledged Put/PutBatch and Fence
// (ADR-0028): UUIDv4 correlation tokens, the 17-byte AckAttachmentV1
// attachment codec, the typed attachment observation, and the AckResultV1
// result codec. See docs/03_wire_protocol.md §6.

#ifndef SITOS_ACK_HPP
#define SITOS_ACK_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sitos/result.hpp"
#include "sitos/status.hpp"
#include "sitos/transport.hpp"

namespace sitos {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

/// True when the token is an RFC 4122 version-4 UUID (version nibble 4,
/// variant bits 10). Only such tokens are valid on the wire or in a query.
[[nodiscard]] bool IsValidAckToken(const AckToken& token) noexcept;

/// Generates a fresh random UUIDv4 token. Tokens are correlation identifiers,
/// not credentials, and are never restored after restart.
[[nodiscard]] AckToken GenerateAckToken();

/// Formats a token as canonical lowercase `8-4-4-4-12` text, the only
/// spelling accepted by `<prefix>/meta/ack/<uuid>`.
[[nodiscard]] std::string FormatAckToken(const AckToken& token);

/// Parses canonical lowercase `8-4-4-4-12` UUIDv4 text. Any other spelling
/// (uppercase, braces, missing hyphens, non-v4) yields nullopt.
[[nodiscard]] std::optional<AckToken> ParseAckToken(std::string_view text);

// ---------------------------------------------------------------------------
// AckAttachmentV1 (exactly 17 bytes: schema_version = 1, then 16 UUID bytes)
// ---------------------------------------------------------------------------

inline constexpr std::size_t kAckAttachmentV1Size = 17;

/// Encodes a token as the exact AckAttachmentV1 wire attachment.
[[nodiscard]] std::array<std::byte, kAckAttachmentV1Size> EncodeAckAttachment(
    const AckToken& token);

/// Classifies a sample's attachment. `nullopt` (no attachment) is Absent; a
/// present attachment is either a valid token or Malformed (unknown version,
/// wrong length, or non-v4 UUID). Never throws.
[[nodiscard]] AckAttachmentObservation ObserveAckAttachment(
    std::optional<std::span<const std::byte>> attachment);

// ---------------------------------------------------------------------------
// AckResultV1
// ---------------------------------------------------------------------------

enum class AckOperationKind : std::uint8_t { Put = 1, Batch = 2, Fence = 3 };

/// The requested durability target; `Status::Ok` confirms it.
enum class AckDurability : std::uint8_t { Applied = 1, Synced = 2 };

inline constexpr std::uint32_t kAckNoFailedIndex = 0xFFFFFFFFu;
inline constexpr std::uint64_t kAckNoFailedSequence = 0xFFFFFFFFFFFFFFFFull;
inline constexpr std::size_t kAckResultV1HeaderSize = 32;
inline constexpr std::size_t kAckResultMaxMessageLength = 1024;

/// Decoded ADR-0028 AckResultV1. Field meanings and per-operation invariants
/// are enforced by ValidateAckResult; `through_sequence == 0` and the
/// `kAckNo*` sentinels mean "not applicable".
struct AckResultV1 {
  AckOperationKind operation_kind = AckOperationKind::Put;
  Status status = Status::Ok;
  AckDurability durability = AckDurability::Applied;
  std::uint32_t applied_count = 0;
  std::uint32_t failed_index = kAckNoFailedIndex;
  std::uint64_t through_sequence = 0;
  std::uint64_t failed_sequence = kAckNoFailedSequence;
  /// Sanitized UTF-8 message, at most kAckResultMaxMessageLength bytes.
  std::string message;

  bool operator==(const AckResultV1&) const = default;
};

/// Checks every ADR-0028 v1 invariant (closed Status allowlist, operation
/// kinds, durability, counts, sentinels, sequences, UTF-8, message length).
/// Returns Status::InvalidArgument with a diagnostic message on violation.
[[nodiscard]] Result<void> ValidateAckResult(const AckResultV1& result);

/// Encodes a validated result as the exact AckResultV1 little-endian layout.
[[nodiscard]] Result<std::vector<std::byte>> EncodeAckResult(const AckResultV1& result);

/// Decodes and validates an AckResultV1 payload. Truncated, trailing, overlong,
/// unknown-version, or invariant-violating data is a protocol error.
[[nodiscard]] Result<AckResultV1> DecodeAckResult(std::span<const std::byte> payload);

}  // namespace sitos

#endif  // SITOS_ACK_HPP
