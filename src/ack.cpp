// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/ack.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace sitos {
namespace {

constexpr std::uint8_t kAttachmentSchemaVersion = 1;
constexpr std::uint8_t kResultSchemaVersion = 1;
constexpr std::string_view kHexDigits = "0123456789abcdef";

std::uint8_t ToU8(std::byte b) { return static_cast<std::uint8_t>(b); }

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;  // uppercase is deliberately not canonical
}

// Strict UTF-8 (RFC 3629): rejects overlong forms, surrogates, code points
// above U+10FFFF, truncated sequences, and stray continuation bytes.
bool IsValidUtf8(std::string_view text) {
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const auto c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
      ++i;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (c0 >= 0xC2 && c0 <= 0xDF) {
      length = 2;
      code_point = c0 & 0x1Fu;
    } else if (c0 >= 0xE0 && c0 <= 0xEF) {
      length = 3;
      code_point = c0 & 0x0Fu;
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
      length = 4;
      code_point = c0 & 0x07u;
    } else {
      return false;  // 0x80..0xC1 and 0xF5..0xFF never start a sequence
    }
    if (i + length > n) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto ck = static_cast<unsigned char>(text[i + k]);
      if ((ck & 0xC0u) != 0x80u) return false;
      code_point = (code_point << 6) | (ck & 0x3Fu);
    }
    if (length == 3 && code_point < 0x800u) return false;              // overlong
    if (length == 4 && code_point < 0x10000u) return false;            // overlong
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) return false;  // surrogate
    if (code_point > 0x10FFFFu) return false;
    i += length;
  }
  return true;
}

bool IsWireStatus(Status status) {
  switch (status) {
    case Status::Ok:
    case Status::NotFound:
    case Status::TypeMismatch:
    case Status::Disconnected:
    case Status::ReadOnly:
    case Status::InvalidKey:
    case Status::InvalidArgument:
    case Status::Error:
    case Status::OutcomeUnknown:
      return true;
    case Status::Timeout:  // client-only; never valid on the wire
      return false;
  }
  return false;  // future or out-of-range values are rejected by v1
}

Result<void> Invalid(std::string message) {
  return Result<void>::Err(Status::InvalidArgument, std::move(message));
}

void PutLe(std::vector<std::byte>& out, std::uint64_t value, int size) {
  for (int i = 0; i < size; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
  }
}

std::uint64_t ReadLe(std::span<const std::byte> in, std::size_t offset, int size) {
  std::uint64_t value = 0;
  for (int i = 0; i < size; ++i) {
    value |= static_cast<std::uint64_t>(ToU8(in[offset + static_cast<std::size_t>(i)])) << (8 * i);
  }
  return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

bool IsValidAckToken(const AckToken& token) noexcept {
  return (ToU8(token.bytes[6]) & 0xF0u) == 0x40u && (ToU8(token.bytes[8]) & 0xC0u) == 0x80u;
}

AckToken GenerateAckToken() {
  std::random_device device;
  AckToken token;
  for (std::size_t i = 0; i < token.bytes.size(); i += 4) {
    const std::uint32_t word = device();
    for (std::size_t k = 0; k < 4; ++k) {
      token.bytes[i + k] = static_cast<std::byte>((word >> (8 * k)) & 0xFFu);
    }
  }
  token.bytes[6] = static_cast<std::byte>((ToU8(token.bytes[6]) & 0x0Fu) | 0x40u);  // version 4
  token.bytes[8] = static_cast<std::byte>((ToU8(token.bytes[8]) & 0x3Fu) | 0x80u);  // RFC 4122
  return token;
}

std::string FormatAckToken(const AckToken& token) {
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < token.bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    const std::uint8_t value = ToU8(token.bytes[i]);
    text.push_back(kHexDigits[value >> 4]);
    text.push_back(kHexDigits[value & 0x0Fu]);
  }
  return text;
}

std::optional<AckToken> ParseAckToken(std::string_view text) {
  if (text.size() != 36) return std::nullopt;
  AckToken token;
  std::size_t byte_index = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return std::nullopt;
      ++i;
      continue;
    }
    const int hi = HexValue(text[i]);
    const int lo = HexValue(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    token.bytes[byte_index++] = static_cast<std::byte>((hi << 4) | lo);
    i += 2;
  }
  if (!IsValidAckToken(token)) return std::nullopt;
  return token;
}

// ---------------------------------------------------------------------------
// AckAttachmentV1
// ---------------------------------------------------------------------------

std::array<std::byte, kAckAttachmentV1Size> EncodeAckAttachment(const AckToken& token) {
  std::array<std::byte, kAckAttachmentV1Size> out{};
  out[0] = std::byte{kAttachmentSchemaVersion};
  for (std::size_t i = 0; i < token.bytes.size(); ++i) out[i + 1] = token.bytes[i];
  return out;
}

AckAttachmentObservation ObserveAckAttachment(
    std::optional<std::span<const std::byte>> attachment) {
  if (!attachment.has_value()) return AckAttachmentAbsent{};
  const std::span<const std::byte> bytes = *attachment;
  if (bytes.size() != kAckAttachmentV1Size) return AckAttachmentMalformed{};
  if (ToU8(bytes[0]) != kAttachmentSchemaVersion) return AckAttachmentMalformed{};
  AckToken token;
  for (std::size_t i = 0; i < token.bytes.size(); ++i) token.bytes[i] = bytes[i + 1];
  if (!IsValidAckToken(token)) return AckAttachmentMalformed{};
  return token;
}

// ---------------------------------------------------------------------------
// AckResultV1
// ---------------------------------------------------------------------------

Result<void> ValidateAckResult(const AckResultV1& r) {
  if (!IsWireStatus(r.status)) return Invalid("ack result status is not a v1 wire status");
  if (r.durability != AckDurability::Applied && r.durability != AckDurability::Synced) {
    return Invalid("ack result durability is unknown");
  }
  if (r.message.size() > kAckResultMaxMessageLength) {
    return Invalid("ack result message exceeds 1024 bytes");
  }
  if (!IsValidUtf8(r.message)) return Invalid("ack result message is not valid UTF-8");

  const bool ok = r.status == Status::Ok;
  const bool no_index = r.failed_index == kAckNoFailedIndex;
  const bool sequences_na = r.through_sequence == 0 && r.failed_sequence == kAckNoFailedSequence;

  switch (r.operation_kind) {
    case AckOperationKind::Put:
      if (r.durability != AckDurability::Applied) return Invalid("put durability must be applied");
      if (!sequences_na) return Invalid("put sequence fields are not applicable");
      if (ok) {
        if (r.applied_count != 1) return Invalid("put success applied_count must be 1");
        if (!no_index) return Invalid("put success has no failed_index");
      } else {
        if (r.applied_count != 0) return Invalid("put failure applied_count must be 0");
        if (r.failed_index != 0) return Invalid("put failure failed_index must be 0");
      }
      return Result<void>::Ok();
    case AckOperationKind::Batch:
      if (r.durability != AckDurability::Applied)
        return Invalid("batch durability must be applied");
      if (!sequences_na) return Invalid("batch sequence fields are not applicable");
      if (ok) {
        if (!no_index) return Invalid("batch success has no failed_index");
      } else if (no_index) {
        if (r.applied_count != 0) return Invalid("batch envelope failure applied_count must be 0");
      } else if (r.applied_count != 0 && r.applied_count != r.failed_index) {
        return Invalid("batch applied_count must be 0 or equal failed_index");
      }
      return Result<void>::Ok();
    case AckOperationKind::Fence:
      if (r.applied_count != 0) return Invalid("fence applied_count must be 0");
      if (!no_index) return Invalid("fence has no failed_index");
      if (r.failed_sequence != kAckNoFailedSequence) {
        if (r.failed_sequence == 0) return Invalid("fence failed_sequence must be nonzero");
        if (r.failed_sequence > r.through_sequence) {
          return Invalid("fence failed_sequence exceeds through_sequence");
        }
      }
      return Result<void>::Ok();
  }
  return Invalid("ack result operation_kind is unknown");
}

Result<std::vector<std::byte>> EncodeAckResult(const AckResultV1& r) {
  if (auto valid = ValidateAckResult(r); !valid.IsOk()) {
    return Result<std::vector<std::byte>>::ErrFrom(valid);
  }
  std::vector<std::byte> out;
  out.reserve(kAckResultV1HeaderSize + r.message.size());
  out.push_back(std::byte{kResultSchemaVersion});
  out.push_back(static_cast<std::byte>(r.operation_kind));
  out.push_back(static_cast<std::byte>(r.status));
  out.push_back(static_cast<std::byte>(r.durability));
  PutLe(out, r.applied_count, 4);
  PutLe(out, r.failed_index, 4);
  PutLe(out, r.through_sequence, 8);
  PutLe(out, r.failed_sequence, 8);
  PutLe(out, r.message.size(), 4);
  for (unsigned char c : r.message) out.push_back(std::byte{c});
  return Result<std::vector<std::byte>>::Ok(std::move(out));
}

Result<AckResultV1> DecodeAckResult(std::span<const std::byte> payload) {
  using R = Result<AckResultV1>;
  if (payload.size() < kAckResultV1HeaderSize) {
    return R::Err(Status::InvalidArgument, "ack result is shorter than the 32-byte header");
  }
  if (ToU8(payload[0]) != kResultSchemaVersion) {
    return R::Err(Status::InvalidArgument, "ack result schema_version is unknown");
  }
  AckResultV1 r;
  r.operation_kind = static_cast<AckOperationKind>(ToU8(payload[1]));
  r.status = static_cast<Status>(ToU8(payload[2]));
  r.durability = static_cast<AckDurability>(ToU8(payload[3]));
  r.applied_count = static_cast<std::uint32_t>(ReadLe(payload, 4, 4));
  r.failed_index = static_cast<std::uint32_t>(ReadLe(payload, 8, 4));
  r.through_sequence = ReadLe(payload, 12, 8);
  r.failed_sequence = ReadLe(payload, 20, 8);
  const auto message_length = static_cast<std::uint32_t>(ReadLe(payload, 28, 4));
  if (message_length > kAckResultMaxMessageLength) {
    return R::Err(Status::InvalidArgument, "ack result message exceeds 1024 bytes");
  }
  if (payload.size() != kAckResultV1HeaderSize + message_length) {
    return R::Err(Status::InvalidArgument, "ack result length does not match message_length");
  }
  r.message.assign(reinterpret_cast<const char*>(payload.data() + kAckResultV1HeaderSize),
                   message_length);
  if (auto valid = ValidateAckResult(r); !valid.IsOk()) return R::ErrFrom(valid);
  return R::Ok(std::move(r));
}

}  // namespace sitos
