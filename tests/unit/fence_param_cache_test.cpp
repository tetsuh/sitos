// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>

#include "fence_test_support.hpp"
#include "param_cache_test_access.hpp"

namespace {

TEST(FenceParamCacheTest, CompletesOnlyTheMatchingAttachGeneration) {
  {
    auto partial_transport = sitos::fence_test::MakeTransport();
    auto partial_cache_result = sitos::fence_test::OpenAttachedCache(partial_transport);
    ASSERT_TRUE(partial_cache_result.IsOk());
    auto partial_cache = std::move(partial_cache_result).Value();
    sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        partial_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);
    sitos::param_cache_test_access::ParamCacheTestAccess::SetMutationHook(
        partial_cache, [](std::size_t count) {
          if (count == 1) throw std::runtime_error("injected partial cache batch failure");
        });

    EXPECT_NO_THROW(
        partial_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
            "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 1,
            {"partial-one", "partial-two"})));
    EXPECT_TRUE(
        sitos::fence_test_access::FenceTestAccess::CacheContains(partial_cache, "partial-one"));
    EXPECT_FALSE(
        sitos::fence_test_access::FenceTestAccess::CacheContains(partial_cache, "partial-two"));
    const auto partial_failure =
        sitos::fence_test_access::FenceTestAccess::CacheFirstFailure(partial_cache);
    EXPECT_EQ(partial_failure.status, sitos::Status::OutcomeUnknown);
    EXPECT_EQ(partial_failure.failed_sequence, 1U);
  }

  {
    auto pre_effect_transport = sitos::fence_test::MakeTransport();
    auto pre_effect_cache_result = sitos::fence_test::OpenAttachedCache(pre_effect_transport);
    ASSERT_TRUE(pre_effect_cache_result.IsOk());
    auto pre_effect_cache = std::move(pre_effect_cache_result).Value();
    sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        pre_effect_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);
    sitos::param_cache_test_access::ParamCacheTestAccess::SetCallbackHook(
        pre_effect_cache, [] { throw std::bad_alloc(); });

    EXPECT_NO_THROW(pre_effect_transport->Deliver(
        sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
            "sitos/session/s1/pre-effect", sitos::fence_test::kPublisherA, 1)));
    EXPECT_FALSE(
        sitos::fence_test_access::FenceTestAccess::CacheContains(pre_effect_cache, "pre-effect"));
    const auto pre_effect_failure =
        sitos::fence_test_access::FenceTestAccess::CacheFirstFailure(pre_effect_cache);
    EXPECT_EQ(pre_effect_failure.status, sitos::Status::Error);
    EXPECT_EQ(pre_effect_failure.failed_sequence, 1U);
  }

  {
    auto failure_transport = sitos::fence_test::MakeTransport();
    auto failure_cache_result = sitos::fence_test::OpenAttachedCache(failure_transport);
    ASSERT_TRUE(failure_cache_result.IsOk());
    auto failure_cache = std::move(failure_cache_result).Value();
    sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        failure_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);

    ASSERT_TRUE(failure_cache.Put("submitted-one", std::int64_t{1}).IsOk());
    ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ThrowCacheDispatchOnce(failure_cache));
    EXPECT_NO_THROW(
        failure_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
            "sitos/session/s1/dropped-two", sitos::fence_test::kPublisherA, 2)));
    auto failure_fence = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
        failure_cache, sitos::fence_test::kDeadline);
    ASSERT_TRUE(failure_fence.IsOk());
    EXPECT_EQ(failure_fence.Value().through_sequence, 1U);
    const auto failure_ack = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(
        sitos::EncodeAckAttachment(failure_fence.Value().token));
    failure_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
        "sitos", "s1", sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA,
        failure_fence.Value().through_sequence, failure_ack));
    const auto failure_result = sitos::fence_test_access::FenceTestAccess::CacheFenceResult(
        failure_cache, failure_fence.Value().token);
    ASSERT_TRUE(failure_result.has_value());
    EXPECT_EQ(failure_result->status, sitos::Status::OutcomeUnknown);
    EXPECT_EQ(failure_result->through_sequence, 1U);
    EXPECT_EQ(failure_result->failed_sequence, 1U);
  }

  auto transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);

  // Decode failure after dispatch admission is a retained sequence-specific
  // Error, not an uncompleted reservation that degrades to OutcomeUnknown.
  std::array<std::byte, 1> malformed_payload{std::byte{0xff}};
  transport->Deliver(
      sitos::TransportSample{"sitos/session/s1/malformed", malformed_payload,
                             sitos::Encoding{std::string(sitos::Encoding::kSitosV1)},
                             sitos::AckAttachmentAbsent{}, sitos::TransportSample::Kind::Put,
                             sitos::FenceLaneMetadata{sitos::fence_test::kPublisherA, 1}});
  const auto malformed_data_failure =
      sitos::fence_test_access::FenceTestAccess::CacheFirstFailure(cache);
  EXPECT_EQ(malformed_data_failure.status, sitos::Status::Error);
  EXPECT_EQ(malformed_data_failure.failed_sequence, 1U);

  // Use a fresh receiver lane for the rest of the prefix/marker matrix.
  sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ThrowCacheDispatchOnce(cache));
  EXPECT_NO_THROW(transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
      "sitos/session/s1/injected-dispatch-failure", sitos::fence_test::kPublisherA, 1)));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::CacheContains(cache, "injected-dispatch-failure"));
  sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA);

  // Sender calls allocate the covered sequences; only their matching real
  // subscriber callbacks advance the receiver proof.
  ASSERT_TRUE(cache.Put("one", std::int64_t{1}).IsOk());
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
      "sitos/session/s1/one", sitos::fence_test::kPublisherA, 1));
  const std::array batch{
      sitos::BatchEntry{"two", sitos::ParamValue(std::int64_t{2})},
      sitos::BatchEntry{"three", sitos::ParamValue(std::int64_t{3})},
  };
  ASSERT_TRUE(cache.PutBatch(batch).IsOk());
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
      "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 2, {"two", "three"}));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::CacheCompletedThrough(cache), 2U);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::CacheMutationCount(cache), 3U);

  const auto queries_before_fence = transport->AckQueryCount();
  auto completed = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(completed.IsOk());
  EXPECT_EQ(completed.Value().through_sequence, 2U);
  const auto ack = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(
      sitos::EncodeAckAttachment(completed.Value().token));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
      "sitos", "s1", sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherB,
      completed.Value().through_sequence, ack));
  EXPECT_TRUE(
      sitos::fence_test_access::FenceTestAccess::CacheFencePending(cache, completed.Value().token));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
      "sitos", "s1", sitos::fence_test::kPublisherA, sitos::fence_test::kPublisherA,
      completed.Value().through_sequence, ack));
  EXPECT_TRUE(
      sitos::fence_test_access::FenceTestAccess::CacheFencePending(cache, completed.Value().token));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
      "sitos", "s1", sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA,
      completed.Value().through_sequence, ack));
  const auto completed_result =
      sitos::fence_test_access::FenceTestAccess::CacheFenceResult(cache, completed.Value().token);
  ASSERT_TRUE(completed_result.has_value());
  EXPECT_EQ(completed_result->status, sitos::Status::Ok);
  EXPECT_EQ(transport->AckQueryCount(), queries_before_fence);
  EXPECT_EQ(transport->StorageNodeResultCount(), 0U);

  auto malformed_route_waiter = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(malformed_route_waiter.IsOk());
  const std::string malformed_route =
      "sitos/meta/fence/cache/s1/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kAttachGeneration) + "/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kPublisherA) + "/02";
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMarkerFromRoute(
      malformed_route, malformed_route_waiter.Value().token));
  const auto malformed_route_result =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(malformed_route_waiter.Value());
  ASSERT_TRUE(malformed_route_result.has_value());
  EXPECT_EQ(malformed_route_result->status, sitos::Status::Error);
  EXPECT_EQ(malformed_route_result->through_sequence, 2U);

  auto trailing_slash_waiter = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(trailing_slash_waiter.IsOk());
  const std::string trailing_slash_route =
      "sitos/meta/fence/cache/s1/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kAttachGeneration) + "/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kPublisherA) + "/2/";
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMarkerFromRoute(
      trailing_slash_route, trailing_slash_waiter.Value().token));
  const auto trailing_slash_result =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(trailing_slash_waiter.Value());
  ASSERT_TRUE(trailing_slash_result.has_value());
  EXPECT_EQ(trailing_slash_result->status, sitos::Status::Error);
  EXPECT_EQ(trailing_slash_result->through_sequence, 2U);

  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
      "sitos/session/s1/duplicate", sitos::fence_test::kPublisherA, 2));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
      "sitos/session/s1/future", sitos::fence_test::kPublisherA, 4));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::CacheContains(cache, "duplicate"));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::CacheContains(cache, "future"));

  // A malformed lane can fail only this identified cache lane. An unidentifiable
  // malformed attachment remains ordinary parameter delivery and cannot claim it.
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMalformedCachePut(
      "sitos/session/s1/four", sitos::fence_test::kPublisherA, 3));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::CacheFirstFailure(cache).status,
            sitos::Status::Error);
  transport->Deliver(
      sitos::fence_test_access::FenceTestAccess::MakeUnidentifiedMalformedParameterPut(
          "sitos/session/s1/five"));
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::CacheContains(cache, "five"));

  // Completion is immutable. Detach cancels only a fresh pending waiter.
  auto pending = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(pending.IsOk());
  EXPECT_TRUE(
      sitos::fence_test_access::FenceTestAccess::CacheFencePending(cache, pending.Value().token));
  cache.Detach();
  // The already-copied completion remains immutable; the detached cache need
  // not retain completed token state. The pending operation owns its cancellation.
  EXPECT_EQ(completed_result->status, sitos::Status::Ok);
  const auto cancelled =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(pending.Value());
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->status, sitos::Status::Disconnected);
  const auto late_ack = sitos::fence_test_access::FenceTestAccess::ClassifyAttachment(
      sitos::EncodeAckAttachment(pending.Value().token));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCacheMarkerSample(
      "sitos", "s1", sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA,
      pending.Value().through_sequence, late_ack));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::CacheFencePending(cache, pending.Value().token));

  ASSERT_TRUE(cache.Attach(sitos::fence_test::kSid).IsOk());
  EXPECT_NE(sitos::fence_test_access::FenceTestAccess::CacheAttachGeneration(cache),
            sitos::fence_test::kAttachGeneration);

  {
    auto capability_transport = sitos::fence_test::MakeTransport();
    auto capability_cache_result = sitos::fence_test::OpenAttachedCache(capability_transport);
    ASSERT_TRUE(capability_cache_result.IsOk());
    auto capability_cache = std::move(capability_cache_result).Value();

    const auto submissions = capability_transport->DataSubmissionCount();
    capability_transport->ReplaceGeneration();
    capability_transport->SetFenceProfileSupported(false);
    const auto first = capability_cache.Put("after-replacement", std::int64_t{1});
    EXPECT_EQ(first.StatusCode(), sitos::Status::Disconnected);
    EXPECT_EQ(capability_transport->DataSubmissionCount(), submissions);
    EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::CacheContains(capability_cache,
                                                                          "after-replacement"));
    const auto second = capability_cache.Put("still-disconnected", std::int64_t{2});
    EXPECT_EQ(second.StatusCode(), sitos::Status::Disconnected);
    EXPECT_EQ(capability_transport->DataSubmissionCount(), submissions);

    capability_cache.Detach();
    auto unsupported_transport = sitos::fence_test::MakeTransport();
    unsupported_transport->SetFenceProfileSupported(false);
    auto unsupported_result = sitos::fence_test::OpenAttachedCache(unsupported_transport);
    ASSERT_TRUE(unsupported_result.IsOk());
    auto unsupported_cache = std::move(unsupported_result).Value();
    EXPECT_TRUE(unsupported_cache.Put("ordinary", std::int64_t{3}).IsOk());
    EXPECT_EQ(unsupported_transport->DataSubmissionCount(), 1U);
  }
}

}  // namespace
