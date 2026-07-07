// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Batch codec for the sitos.v1.batch payload. See docs/03_wire_protocol.md §5.

#include "sitos/batch.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace sitos {

namespace {

void AppendLeU32(std::uint32_t v, std::vector<std::byte>& out) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
  }
}

std::optional<std::uint32_t> ReadLeU32(std::span<const std::byte> p, std::size_t& off) {
  if (off + 4 > p.size()) return std::nullopt;
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[off + i])) << (8 * i);
  }
  off += 4;
  return v;
}

}  // namespace

std::vector<std::byte> EncodeBatch(std::span<const std::pair<std::string, ParamValue>> entries) {
  std::vector<std::byte> out;
  AppendLeU32(static_cast<std::uint32_t>(entries.size()), out);
  for (const auto& [key, value] : entries) {
    AppendLeU32(static_cast<std::uint32_t>(key.size()), out);
    const auto* kp = reinterpret_cast<const std::byte*>(key.data());
    out.insert(out.end(), kp, kp + key.size());
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value.type())));
    auto full = value.Encode();
    // full[0] is the type tag (already written); body is full[1..].
    const auto body_len = static_cast<std::uint32_t>(full.size() - 1);
    AppendLeU32(body_len, out);
    out.insert(out.end(), full.begin() + 1, full.end());
  }
  return out;
}

std::optional<std::vector<BatchEntry>> DecodeBatch(std::span<const std::byte> payload) {
  std::size_t off = 0;
  auto count = ReadLeU32(payload, off);
  if (!count) return std::nullopt;
  // A single entry is at least 4 (kLen) + 0 (key) + 1 (tag) + 4 (vLen) = 9 bytes.
  if (*count > payload.size() / 9 + 1) return std::nullopt;

  std::vector<BatchEntry> result;
  result.reserve(std::min<std::size_t>(*count, payload.size()));
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto k_len = ReadLeU32(payload, off);
    if (!k_len) return std::nullopt;
    // Use remaining-capacity form to avoid unsigned wraparound on `off + *k_len`.
    if (*k_len > payload.size() - off) return std::nullopt;
    std::string key(reinterpret_cast<const char*>(payload.data() + off), *k_len);
    off += *k_len;

    if (off >= payload.size()) return std::nullopt;
    std::uint8_t tag = static_cast<std::uint8_t>(payload[off]);
    off += 1;

    auto v_len = ReadLeU32(payload, off);
    if (!v_len) return std::nullopt;
    // Same remaining-capacity form for `off + *v_len`.
    if (*v_len > payload.size() - off) return std::nullopt;

    // ParamValue::Decode expects tag + body; reassemble a single-value payload.
    std::vector<std::byte> single;
    single.reserve(1 + *v_len);
    single.push_back(static_cast<std::byte>(tag));
    single.insert(single.end(), payload.data() + off, payload.data() + off + *v_len);
    off += *v_len;

    auto value = ParamValue::Decode(single);
    if (!value) return std::nullopt;
    result.push_back({std::move(key), std::move(*value)});
  }
  if (off != payload.size()) return std::nullopt;
  return result;
}

}  // namespace sitos