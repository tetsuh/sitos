// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "fence_test_support.hpp"

namespace {

TEST(FenceCollisionTest, PinsDocumentedUuidAndTokenResidualBoundaries) {
  // Receiver state is keyed by Publisher UUID. A forced same-UUID next-expected
  // raw write is deliberately indistinguishable from the supported Publisher;
  // this is the documented residual risk, not a claimed collision detector.
  auto receiver = sitos::fence_test_access::FenceTestAccess::CreateReceiver();
  ASSERT_TRUE(receiver.ObserveRaw(sitos::fence_test::kPublisherA, 1).IsOk());
  ASSERT_TRUE(receiver.ObserveRaw(sitos::fence_test::kPublisherA, 2).IsOk());
  EXPECT_EQ(receiver.CompletedThrough(sitos::fence_test::kPublisherA), 2U);
  ASSERT_TRUE(receiver.ObserveRaw(sitos::fence_test::kPublisherB, 1).IsOk());
  EXPECT_EQ(receiver.CompletedThrough(sitos::fence_test::kPublisherB), 1U);
  EXPECT_EQ(receiver.CompletedThrough(sitos::fence_test::kPublisherA), 2U);

  // ADR-0028's real registry establishes the two observable token-collision
  // cases used by Fence: same fingerprint is retained, different fingerprint is
  // rejected without replacing the original immutable result.
  sitos::AckRegistry registry;
  const auto token = sitos::fence_test::Token(std::byte{0x51});
  const std::array<std::byte, 1> payload{std::byte{0x01}};
  const auto original = sitos::ComputeAckFingerprint(sitos::AckOperationKind::Fence,
                                                     "sitos/meta/fence/buffer/s1/g/d/p/applied/1",
                                                     sitos::Encoding::kSitosV1Fence, payload);
  const auto different = sitos::ComputeAckFingerprint(sitos::AckOperationKind::Fence,
                                                      "sitos/meta/fence/buffer/s1/g/d/p/applied/2",
                                                      sitos::Encoding::kSitosV1Fence, payload);
  ASSERT_EQ(registry.Claim(token, original, 17), sitos::AckRegistry::ClaimOutcome::Admitted);
  const auto retained =
      sitos::fence_test::FenceResult(sitos::Status::Ok, sitos::AckDurability::Applied, 1);
  ASSERT_TRUE(registry.Complete(token, retained));
  EXPECT_EQ(registry.Claim(token, original, 17),
            sitos::AckRegistry::ClaimOutcome::DuplicateCompleted);
  EXPECT_EQ(registry.Find(token), retained);
  EXPECT_EQ(registry.Claim(token, different, 17), sitos::AckRegistry::ClaimOutcome::Collision);
  EXPECT_EQ(registry.Find(token), retained);

  // A cache replacement may generate the same token only in a forced residual
  // test. The old waiter is already terminally Disconnected and the marker can
  // complete only the replacement waiter's operation-owned state.
  auto cache_transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(cache_transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
  const auto forced_cache_token = sitos::fence_test::Token(std::byte{0x53});
  auto old_waiter = sitos::fence_test_access::FenceTestAccess::PublishCacheWaiterWithToken(
      cache, 0, sitos::fence_test::kDeadline, forced_cache_token);
  ASSERT_TRUE(old_waiter.IsOk());
  cache.Detach();
  const auto old_waiter_result =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(old_waiter.Value());
  ASSERT_TRUE(old_waiter_result.has_value());
  EXPECT_EQ(old_waiter_result->status, sitos::Status::Disconnected);

  ASSERT_TRUE(cache.Attach(sitos::fence_test::kSid).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
  auto replacement_waiter = sitos::fence_test_access::FenceTestAccess::PublishCacheWaiterWithToken(
      cache, 0, sitos::fence_test::kDeadline, forced_cache_token);
  ASSERT_TRUE(replacement_waiter.IsOk());
  const auto forced_ack = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(
      sitos::EncodeAckAttachment(forced_cache_token));
  cache_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
      "sitos", "s1", sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA, 0,
      forced_ack));
  const auto replacement_result =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(replacement_waiter.Value());
  ASSERT_TRUE(replacement_result.has_value());
  EXPECT_EQ(replacement_result->status, sitos::Status::Ok);
  EXPECT_EQ(
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(old_waiter.Value())->status,
      sitos::Status::Disconnected);

  auto transport = sitos::fence_test::MakeTransport();
  auto node = sitos::fence_test::StartNode(transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->CreateSession("s1", sitos::fence_test::DurableSessionOptions()).IsOk());
  sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "s1", sitos::fence_test::kSessionGeneration);
  const auto old_token = sitos::fence_test::Token(std::byte{0x52});
  ASSERT_TRUE(node->CloseSession("s1").IsOk());
  ASSERT_TRUE(node->CreateSession("s1", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "s1", sitos::fence_test::kAttachGeneration));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, old_token));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, old_token).has_value())
      << "a delayed old-generation marker cannot complete in a fresh Session generation";

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::CloseAndRecreateSession(
      *node, "s1", sitos::fence_test::DurableSessionOptions(),
      sitos::fence_test::kSessionGeneration));
  // A forced repeated Session UUID is a documented residual risk. The test pins
  // that the implementation neither calls it impossible nor revives an evicted
  // old token without the normal registry/fingerprint rules.
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, old_token));
  EXPECT_TRUE(
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, old_token).has_value());
  EXPECT_EQ(
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, old_token)->operation_kind,
      sitos::AckOperationKind::Fence);
}

}  // namespace
