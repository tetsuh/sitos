// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "fence_test_support.hpp"

namespace {

using sitos::AckAttachmentAbsent;
using sitos::AckToken;
using sitos::FenceLaneAbsent;
using sitos::FenceLaneMalformed;
using sitos::FenceLaneMetadata;
using sitos::ParamCache;
using sitos::fence_test::kAttachGeneration;
using sitos::fence_test::kPrefix;
using sitos::fence_test::kPublisherA;
using sitos::fence_test::kPublisherB;
using sitos::fence_test::kSessionGeneration;
using sitos::fence_test::kSid;

std::array<std::byte, 17> ValidAckBytes() {
  return sitos::EncodeAckAttachment(sitos::fence_test::Token(std::byte{0x40}));
}

std::array<std::byte, 25> ValidLaneBytes() {
  return sitos::fence_test_access::FenceTestAccess::EncodeLane(
      FenceLaneMetadata{.publisher_uuid = kPublisherA, .sequence = 7});
}

TEST(FenceAdapterTest, ClassifiesAttachmentsByRouteWithoutDroppingParameters) {
  const auto ack_bytes = ValidAckBytes();
  const auto lane_bytes = ValidLaneBytes();
  const auto ack = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(ack_bytes);
  ASSERT_TRUE(std::holds_alternative<AckToken>(ack.ack));
  EXPECT_TRUE(std::holds_alternative<FenceLaneAbsent>(ack.lane));

  const auto lane = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(lane_bytes);
  EXPECT_TRUE(std::holds_alternative<AckAttachmentAbsent>(lane.ack));
  ASSERT_TRUE(std::holds_alternative<FenceLaneMetadata>(lane.lane));
  EXPECT_EQ(std::get<FenceLaneMetadata>(lane.lane).sequence, 7U);

  auto short_v1 = std::vector<std::byte>(lane_bytes.begin(), lane_bytes.end() - 1);
  auto raw_non_v1 = std::vector<std::byte>(24, std::byte{0x00});
  EXPECT_TRUE(std::holds_alternative<FenceLaneMalformed>(
      sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(short_v1).lane));
  EXPECT_TRUE(std::holds_alternative<FenceLaneAbsent>(
      sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(raw_non_v1).lane));

  auto transport = sitos::fence_test::MakeTransport();
  auto node = sitos::fence_test::StartNode(transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->CreateSession(kSid).IsOk());

  const auto parameter = sitos::fence_test_access::FenceTestAccess::MakeSample(
      "sitos/session/s1/value", lane, sitos::TransportSample::Kind::Put);
  const auto raw_parameter = sitos::fence_test_access::FenceTestAccess::MakeSample(
      "sitos/session/s1/raw",
      sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(raw_non_v1),
      sitos::TransportSample::Kind::Put);
  transport->Deliver(parameter);
  transport->Deliver(raw_parameter);
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::ParameterWasApplied(*node, "value"));
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::ParameterWasApplied(*node, "raw"));

  for (const auto& route :
       {"sitos/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/7",
        "sitos/meta/fence/buffer/s1/6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab/durable/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/applied/0"}) {
    EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::ParseMarkerRoute(route).has_value());
  }
  for (const auto& route :
       {"sitos/meta/fence/cache/s1/123E4567-e89b-42d3-a456-426614174000/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/7",
        "sitos/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/07",
        "sitos/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/-1",
        "sitos/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/18446744073709551616",
        "sitos/meta/fence/buffer/s1/6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab/ephemeral/"
        "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/synced/7"}) {
    EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::ParseMarkerRoute(route).has_value());
  }
}

}  // namespace
