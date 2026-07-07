// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Tests for ParamValue payload v1 codec and the sitos.v1.batch codec.
// Required test names (docs/06 §4.1): PayloadV1GoldenFixtures, BatchV1GoldenFixture.

#include "sitos/param_value.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "sitos/batch.hpp"

namespace {

// Parse a `.hex` file (whitespace-separated hex pairs) into bytes.
std::vector<std::byte> ReadFixtureHex(const std::string& name) {
  const std::string path = std::string(SITOS_FIXTURE_DIR) + "/" + name;
  std::ifstream f(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string hex;
  for (char c : content) {
    if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
  }
  EXPECT_FALSE(hex.empty()) << "fixture " << name << " is empty / missing";
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16))));
  }
  return out;
}

bool BytesEq(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace

// docs/06 §4.1: PayloadV1GoldenFixtures — exact match with docs/03 §2.3.
TEST(PayloadV1GoldenFixtures, SingleValuesMatchSpecByteForByte) {
  struct Case {
    const char* fixture;
    sitos::ParamValue value;
  };
  // Note: NaN is constructed with a non-canonical payload to also exercise
  // canonical normalization in the same loop.
  double noncanonical_nan = []() {
    std::uint64_t bits = 0x7ff8000000000001ull;  // NaN with payload
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }();

  std::vector<Case> cases = {
      {"bool_false.hex", sitos::ParamValue(false)},
      {"bool_true.hex", sitos::ParamValue(true)},
      {"s64_zero.hex", sitos::ParamValue(std::int64_t{0})},
      {"s64_minus1.hex", sitos::ParamValue(std::int64_t{-1})},
      {"s64_i32max.hex", sitos::ParamValue(std::int64_t{2147483647})},
      {"dp_zero.hex", sitos::ParamValue(0.0)},
      {"dp_240.hex", sitos::ParamValue(240.0)},
      {"dp_nan.hex", sitos::ParamValue(noncanonical_nan)},
      {"str_empty.hex", sitos::ParamValue(std::string(""))},
      {"str_ascii.hex", sitos::ParamValue(std::string("abc"))},
      {"str_utf8.hex", sitos::ParamValue(std::string("穀"))},
      {"bytes_empty.hex", sitos::ParamValue(std::vector<std::byte>{})},
      {"bytes_0102ff.hex", sitos::ParamValue(std::vector<std::byte>{
                               std::byte{0x01}, std::byte{0x02}, std::byte{0xff}})},
  };

  for (const auto& c : cases) {
    auto expected = ReadFixtureHex(c.fixture);
    auto encoded = c.value.Encode();
    EXPECT_TRUE(BytesEq(encoded, expected))
        << c.fixture << ": encoded " << encoded.size() << "B, expected " << expected.size() << "B";

    auto decoded = sitos::ParamValue::Decode(expected);
    ASSERT_TRUE(decoded.has_value()) << c.fixture << ": decode failed";
    EXPECT_EQ(decoded->type(), c.value.type()) << c.fixture << ": decoded type mismatch";
  }
}

// docs/03 §2.3: NaN must normalize to the canonical pattern (0x7ff8...0000).
TEST(PayloadV1GoldenFixtures, NanIsCanonicalOnReencode) {
  std::uint64_t bits = 0x7fff000000000123ull;  // NaN, different payload
  double d;
  std::memcpy(&d, &bits, sizeof(d));
  ASSERT_TRUE(std::isnan(d));

  auto encoded = sitos::ParamValue(d).Encode();
  auto canonical = ReadFixtureHex("dp_nan.hex");
  EXPECT_TRUE(BytesEq(encoded, canonical))
      << "NaN was not normalized to the canonical byte pattern";
}

// docs/06 §4.1: BatchV1GoldenFixture — exact match with docs/03 §5.1.
TEST(BatchV1GoldenFixture, EncodesAndDecodesTheSpecFixture) {
  std::vector<std::pair<std::string, sitos::ParamValue>> entries = {
      {"recon/fov", sitos::ParamValue(240.0)},
      {"recon/kernel", sitos::ParamValue(std::string("sharp"))},
  };
  auto encoded = sitos::EncodeBatch(entries);
  auto expected = ReadFixtureHex("batch_base_two_entries.hex");
  EXPECT_TRUE(BytesEq(encoded, expected))
      << "encoded " << encoded.size() << "B, expected " << expected.size() << "B";

  auto decoded = sitos::DecodeBatch(expected);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), 2u);
  EXPECT_EQ((*decoded)[0].key, "recon/fov");
  EXPECT_EQ((*decoded)[0].value.type(), sitos::ValueType::Dp);
  EXPECT_EQ((*decoded)[0].value.As<double>(), 240.0);
  EXPECT_EQ((*decoded)[1].key, "recon/kernel");
  EXPECT_EQ((*decoded)[1].value.type(), sitos::ValueType::Str);
  EXPECT_EQ((*decoded)[1].value.As<std::string>(), "sharp");

  // Round-trip: re-encoding the decoded entries reproduces the exact bytes.
  std::vector<std::pair<std::string, sitos::ParamValue>> reencoded_entries;
  for (auto& e : *decoded) {
    reencoded_entries.emplace_back(e.key, e.value);
  }
  auto reencoded = sitos::EncodeBatch(reencoded_entries);
  EXPECT_TRUE(BytesEq(reencoded, expected)) << "batch round-trip drifted";
}

// Round-trip coverage for all five types, including boundary values.
TEST(ParamValue, RoundTripsAllTypesAndBoundaries) {
  auto roundtrip = [](const sitos::ParamValue& v) {
    auto encoded = v.Encode();
    auto decoded = sitos::ParamValue::Decode(encoded);
    if (!decoded.has_value()) return decoded;
    // Re-encode and compare bytes to catch non-idempotent encoding.
    auto reencoded = decoded->Encode();
    bool stable = BytesEq(encoded, reencoded);
    if (!stable) decoded = std::nullopt;
    return decoded;
  };

  // BOOL
  {
    auto r = roundtrip(sitos::ParamValue(false));
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->As<bool>().has_value());
    EXPECT_FALSE(*r->As<bool>());
  }
  {
    auto r = roundtrip(sitos::ParamValue(true));
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->As<bool>().has_value());
    EXPECT_TRUE(*r->As<bool>());
  }

  // S64 boundaries
  for (std::int64_t v :
       {std::int64_t{0}, std::int64_t{-1}, std::int64_t{2147483647},
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
    auto r = roundtrip(sitos::ParamValue(v));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->type(), sitos::ValueType::S64);
    EXPECT_EQ(*r->As<std::int64_t>(), v);
  }

  // DP boundaries
  for (double v :
       {0.0, 240.0, 1e-300, 1e300, std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    auto r = roundtrip(sitos::ParamValue(v));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->type(), sitos::ValueType::Dp);
    EXPECT_EQ(*r->As<double>(), v);
  }

  // NaN round-trip (byte stability, via canonical normalization)
  {
    double nan = std::numeric_limits<double>::quiet_NaN();
    auto r = roundtrip(sitos::ParamValue(nan));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(std::isnan(*r->As<double>()));
  }

  // STR
  for (const std::string& s : {std::string(""), std::string("abc"), std::string("穀"),
                               // length-prefixed construction so the embedded
                               // NUL is kept (the const char* ctor would truncate).
                               std::string("multi\nline\x00with\tnul", 19)}) {
    auto r = roundtrip(sitos::ParamValue(s));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->type(), sitos::ValueType::Str);
    EXPECT_EQ(*r->As<std::string>(), s);
  }

  // BYTES
  for (const std::vector<std::byte>& b :
       {std::vector<std::byte>{},
        std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0xff}}}) {
    auto r = roundtrip(sitos::ParamValue(b));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->type(), sitos::ValueType::Bytes);
    EXPECT_EQ(*r->As<std::vector<std::byte>>(), b);
  }
}

// Arithmetic casts among numerical types are allowed; impossible combos give
// std::nullopt.
TEST(ParamValue, ArithmeticCastsAmongNumericalTypes) {
  sitos::ParamValue s64{std::int64_t{7}};
  EXPECT_EQ(*s64.As<int>(), 7);
  EXPECT_EQ(*s64.As<double>(), 7.0);
  EXPECT_EQ(*s64.As<bool>(), true);

  sitos::ParamValue dp{2.5};
  EXPECT_EQ(*dp.As<int64_t>(), 2);
  EXPECT_EQ(*dp.As<double>(), 2.5);

  // String cannot be extracted as int.
  sitos::ParamValue str{std::string("x")};
  EXPECT_FALSE(str.As<int>().has_value());
  EXPECT_TRUE(str.As<std::string>().has_value());

  // int cannot be extracted as bytes.
  sitos::ParamValue by{std::vector<std::byte>{std::byte{0x1}}};
  EXPECT_FALSE(by.As<int>().has_value());
  EXPECT_FALSE(by.As<std::string>().has_value());
}

// AsSpan is zero-copy over the Bytes buffer with size-alignment check.
TEST(ParamValue, AsSpanIsZeroCopyAndAligned) {
  std::vector<std::byte> raw{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                             std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}};
  sitos::ParamValue v{raw};
  auto span = v.AsSpan<std::uint32_t>();
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->size(), 2u);
  EXPECT_EQ(span->data()[0], 0x03020100u);  // little-endian read-back of raw[0..3]
  // zero-copy: repeated calls return a view over the same stable internal
  // buffer (no per-call copy).
  auto span2 = v.AsSpan<std::uint32_t>();
  ASSERT_TRUE(span2.has_value());
  EXPECT_EQ(span->data(), span2->data());

  // Non-Bytes returns nullopt.
  sitos::ParamValue s{std::int64_t{1}};
  EXPECT_FALSE(s.AsSpan<std::uint32_t>().has_value());

  // Misaligned size returns nullopt.
  raw.push_back(std::byte{0x08});  // 9 bytes, not a multiple of 4
  sitos::ParamValue v2{raw};
  EXPECT_FALSE(v2.AsSpan<std::uint32_t>().has_value());

  // Empty bytes yields an empty span, not nullopt.
  sitos::ParamValue empty_b{std::vector<std::byte>{}};
  auto es = empty_b.AsSpan<std::uint32_t>();
  ASSERT_TRUE(es.has_value());
  EXPECT_EQ(es->size(), 0u);
}

// Invalid payloads are rejected without UB (verified under ASan/UBSan in CI).
TEST(ParamValue, RejectsInvalidPayloads) {
  auto expect_invalid = [](const std::vector<std::byte>& payload) {
    EXPECT_FALSE(sitos::ParamValue::Decode(payload).has_value())
        << "payload of " << payload.size() << "B should be invalid";
  };

  expect_invalid({});                            // empty
  expect_invalid({std::byte{0x00}});             // truncated S64 (tag only)
  for (std::uint8_t tag = 5; tag <= 7; ++tag) {  // reserved range
    expect_invalid({std::byte{tag}});
  }
  expect_invalid({std::byte{0xFF}});  // unused tag
  // truncated S64 body (only 4 bytes)
  expect_invalid({std::byte{0x01}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
  // S64 body too long (extra byte)
  expect_invalid({std::byte{0x01}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                  std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
  // truncated DP body
  expect_invalid({std::byte{0x02}, std::byte{0}});
  // bool body != 1 byte
  expect_invalid({std::byte{0x00}, std::byte{0x01}, std::byte{0x02}});

  // But note: STR/BYTES with zero-length body are VALID (str_empty, bytes_empty).
  ASSERT_TRUE(sitos::ParamValue::Decode(std::vector<std::byte>{std::byte{0x03}}).has_value());
  ASSERT_TRUE(sitos::ParamValue::Decode(std::vector<std::byte>{std::byte{0x04}}).has_value());
}

// Batch rejects malformed input.
TEST(ParamValue, RejectsMalformedBatches) {
  auto expect_invalid = [](const std::vector<std::byte>& p) {
    EXPECT_FALSE(sitos::DecodeBatch(std::span<const std::byte>{p.data(), p.size()}).has_value());
  };
  expect_invalid({});                                  // empty
  expect_invalid({std::byte{0x02}, std::byte{0x00}});  // truncated count (4B)
  // count=1 but no entry
  expect_invalid({std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});
  // good count + key length but key truncated
  expect_invalid({std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                  std::byte{0x09}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});
  // trailing bytes after a fully valid entry (count=1, kLen=0, tag=BOOL,
  // vLen=1, body=0x00, then an extra 0xFF). This reaches the trailing-bytes
  // guard rather than failing earlier at the key-length check.
  expect_invalid({std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                  std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                  std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                  std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}});
  // unknown value tag inside a syntactically well-formed entry (count=1,
  // kLen=0, tag=0xFE, vLen=0). This reaches ParamValue::Decode's unknown-tag
  // rejection rather than the key-length guard.
  std::vector<std::byte> bad{std::byte{0x01}, std::byte{0}, std::byte{0}, std::byte{0},
                             std::byte{0},    std::byte{0}, std::byte{0}, std::byte{0},
                             std::byte{0xFE}, std::byte{0}, std::byte{0}, std::byte{0},
                             std::byte{0}};
  expect_invalid(bad);
}