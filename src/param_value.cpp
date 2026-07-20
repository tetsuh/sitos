// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ParamValue payload v1 codec implementation. See docs/03_wire_protocol.md §2.

#include "sitos/param_value.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sitos {

namespace {

void AppendLeU64(std::uint64_t u, std::vector<std::byte>& out) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((u >> (8 * i)) & 0xff));
  }
}

// Canonical quiet-NaN body bytes (docs/03 §2.3 dp_nan). Already in LE order.
// Corresponds to the bit pattern 0x7ff8000000000000.
constexpr std::array<std::byte, 8> kCanonicalNanBody = {
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0xf8}, std::byte{0x7f}};

}  // namespace

std::vector<std::byte> ParamValue::EncodeBody() const {
  std::vector<std::byte> body;
  switch (value_.index()) {
    case 0: {  // Bool
      body.push_back(std::get<bool>(value_) ? std::byte{0x01} : std::byte{0x00});
      break;
    }
    case 1: {  // S64
      std::int64_t s = std::get<std::int64_t>(value_);
      std::uint64_t u = 0;
      std::memcpy(&u, &s, sizeof(u));
      AppendLeU64(u, body);
      break;
    }
    case 2: {  // DP (normalize NaN to canonical pattern)
      double d = std::get<double>(value_);
      if (std::isnan(d)) {
        body.resize(kCanonicalNanBody.size());
        std::memcpy(body.data(), kCanonicalNanBody.data(), body.size());
      } else {
        std::uint64_t u = 0;
        std::memcpy(&u, &d, sizeof(u));
        AppendLeU64(u, body);
      }
      break;
    }
    case 3: {  // Str
      const auto& s = std::get<std::string>(value_);
      if (!s.empty()) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        body.insert(body.end(), p, p + s.size());
      }
      break;
    }
    case 4: {  // Bytes
      const auto& b = std::get<std::vector<std::byte>>(value_);
      if (!b.empty()) {
        body.insert(body.end(), b.begin(), b.end());
      }
      break;
    }
    default:
      break;  // Unreachable: variant holds one of the 5 documented types.
  }
  return body;
}

std::vector<std::byte> ParamValue::Encode() const {
  auto body = EncodeBody();
  std::vector<std::byte> out;
  if (!body.empty()) {
    out.reserve(1 + body.size());
  }
  out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(type())));
  if (!body.empty()) {
    out.insert(out.end(), body.begin(), body.end());
  }
  return out;
}

std::optional<ParamValue> ParamValue::Decode(std::span<const std::byte> payload) {
  if (payload.empty()) return std::nullopt;
  return Decode(static_cast<std::uint8_t>(payload[0]), payload.subspan(1));
}

std::optional<ParamValue> ParamValue::Decode(std::uint8_t tag,
                                             std::span<const std::byte> body) {
  switch (tag) {
    case 0: {  // Bool
      if (body.size() != 1) return std::nullopt;
      return ParamValue(body[0] != std::byte{0});
    }
    case 1: {  // S64
      if (body.size() != 8) return std::nullopt;
      std::uint64_t u = 0;
      for (int i = 0; i < 8; ++i) {
        u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(body[i])) << (8 * i);
      }
      std::int64_t s = 0;
      std::memcpy(&s, &u, sizeof(s));
      return ParamValue(s);
    }
    case 2: {  // DP
      if (body.size() != 8) return std::nullopt;
      std::uint64_t u = 0;
      for (int i = 0; i < 8; ++i) {
        u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(body[i])) << (8 * i);
      }
      double d = 0;
      std::memcpy(&d, &u, sizeof(d));
      return ParamValue(d);
    }
    case 3: {  // Str
      std::string s(reinterpret_cast<const char*>(body.data()), body.size());
      return ParamValue(std::move(s));
    }
    case 4: {  // Bytes
      std::vector<std::byte> b(body.begin(), body.end());
      return ParamValue(std::move(b));
    }
    default:
      // 5..127 reserved, 128..255 unused: reject.
      return std::nullopt;
  }
}

}  // namespace sitos