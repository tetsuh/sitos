// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "fence_test_support.hpp"

namespace {

using sitos::FenceLaneAbsent;
using sitos::FenceLaneMalformed;
using sitos::FenceLaneMetadata;
using sitos::fence_test::kPublisherA;

std::array<std::byte, 25> GoldenLane(std::uint64_t sequence) {
  std::array<std::byte, 25> bytes{{
      std::byte{0x01}, std::byte{0x8b}, std::byte{0x8f}, std::byte{0x3a}, std::byte{0x62},
      std::byte{0x7d}, std::byte{0xd5}, std::byte{0x4c}, std::byte{0x40}, std::byte{0x8a},
      std::byte{0x2b}, std::byte{0x28}, std::byte{0xf7}, std::byte{0x13}, std::byte{0x31},
      std::byte{0xfe}, std::byte{0x41}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  }};
  for (std::size_t index = 0; index != 8; ++index) {
    bytes[17 + index] = static_cast<std::byte>((sequence >> (index * 8)) & 0xffU);
  }
  return bytes;
}

TEST(FenceLaneCodecTest, GoldenAndNegativeForms) {
  const FenceLaneMetadata min{.publisher_uuid = kPublisherA, .sequence = 1};
  const FenceLaneMetadata max{.publisher_uuid = kPublisherA, .sequence = UINT64_MAX};
  const auto golden_min = GoldenLane(1);
  const auto golden_max = GoldenLane(UINT64_MAX);

  // Independently generated fixture bytes prevent the C++ implementation and
  // test from agreeing on the same incorrect self-derived layout.
  EXPECT_EQ(sitos::fence_test::ReadHexFixture("lane_min_sequence.hex"),
            std::vector<std::byte>(golden_min.begin(), golden_min.end()));
  EXPECT_EQ(sitos::fence_test::ReadHexFixture("lane_max_sequence.hex"),
            std::vector<std::byte>(golden_max.begin(), golden_max.end()));
  EXPECT_EQ(sitos::fence_test::ReadHexFixture("marker_v1.hex"),
            (std::vector<std::byte>{std::byte{0x01}}));

  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::EncodeLane(min), golden_min);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::EncodeLane(max), golden_max);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::EncodeMarker(),
            (std::array<std::byte, 1>{std::byte{0x01}}));

  for (const auto& [name, bytes, expected_sequence] :
       std::array{std::tuple{"minimum", golden_min, std::uint64_t{1}},
                  std::tuple{"maximum", golden_max, UINT64_MAX}}) {
    SCOPED_TRACE(name);
    const auto observed = sitos::fence_test_access::FenceTestAccess::ObserveLane(bytes);
    ASSERT_TRUE(std::holds_alternative<FenceLaneMetadata>(observed));
    EXPECT_EQ(std::get<FenceLaneMetadata>(observed).publisher_uuid, kPublisherA);
    EXPECT_EQ(std::get<FenceLaneMetadata>(observed).sequence, expected_sequence);
  }

  auto zero_sequence = golden_min;
  zero_sequence.fill(std::byte{0x00});
  zero_sequence[0] = std::byte{0x01};
  std::copy(kPublisherA.begin(), kPublisherA.end(), zero_sequence.begin() + 1);
  auto invalid_version = golden_min;
  invalid_version[0] = std::byte{0x02};
  auto invalid_uuid_variant = golden_min;
  invalid_uuid_variant[9] = std::byte{0x0a};
  auto invalid_uuid_version = golden_min;
  invalid_uuid_version[7] = std::byte{0x5c};
  auto short_v1 = std::vector<std::byte>(24, std::byte{0x00});
  short_v1.front() = std::byte{0x01};
  auto long_v1 = std::vector<std::byte>(26, std::byte{0x00});
  long_v1.front() = std::byte{0x01};

  const std::array<std::pair<std::string_view, std::span<const std::byte>>, 6> malformed_rows{{
      {"zero sequence", std::span<const std::byte>(zero_sequence)},
      {"unknown version", std::span<const std::byte>(invalid_version)},
      {"invalid UUID variant", std::span<const std::byte>(invalid_uuid_variant)},
      {"invalid UUID version", std::span<const std::byte>(invalid_uuid_version)},
      {"24-byte v1", std::span<const std::byte>(short_v1)},
      {"26-byte v1", std::span<const std::byte>(long_v1)},
  }};
  for (const auto& [name, bytes] : malformed_rows) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(std::holds_alternative<FenceLaneMalformed>(
        sitos::fence_test_access::FenceTestAccess::ObserveLane(bytes)));
  }

  const std::array<std::byte, 24> raw_short{};
  const std::array<std::byte, 26> raw_long{};
  EXPECT_TRUE(std::holds_alternative<FenceLaneAbsent>(
      sitos::fence_test_access::FenceTestAccess::ObserveLane(raw_short)));
  EXPECT_TRUE(std::holds_alternative<FenceLaneAbsent>(
      sitos::fence_test_access::FenceTestAccess::ObserveLane(raw_long)));

  const auto uuid_only = sitos::fence_test_access::FenceTestAccess::ObserveLane(short_v1);
  ASSERT_TRUE(std::holds_alternative<FenceLaneMalformed>(uuid_only));
  EXPECT_FALSE(std::get<FenceLaneMalformed>(uuid_only).publisher_uuid.has_value());
  EXPECT_FALSE(std::get<FenceLaneMalformed>(uuid_only).sequence.has_value());
  const auto zero = sitos::fence_test_access::FenceTestAccess::ObserveLane(zero_sequence);
  ASSERT_TRUE(std::holds_alternative<FenceLaneMalformed>(zero));
  EXPECT_EQ(std::get<FenceLaneMalformed>(zero).publisher_uuid, kPublisherA);
  EXPECT_FALSE(std::get<FenceLaneMalformed>(zero).sequence.has_value());

  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::DecodeMarker(
                  std::array<std::byte, 1>{std::byte{0x01}})
                  .IsOk());
  for (const auto& bytes : {std::vector<std::byte>{}, std::vector<std::byte>{std::byte{0x02}},
                            std::vector<std::byte>{std::byte{0x01}, std::byte{0x00}}}) {
    EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::DecodeMarker(bytes).IsOk());
  }
}

}  // namespace
