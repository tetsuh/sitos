// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "fence_test_support.hpp"
#include "param_cache_test_access.hpp"

namespace {

TEST(FenceParamCacheTest, CompletesOnlyTheMatchingAttachGeneration) {
  std::vector<std::byte> first_batch_storage;
  auto first_batch = sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
      "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 1, first_batch_storage,
      {"first-batch"});
  const std::vector first_batch_copy(first_batch.payload.begin(), first_batch.payload.end());
  std::vector<std::byte> second_batch_storage;
  auto second_batch = sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
      "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 2, second_batch_storage,
      {"second-batch"});
  EXPECT_EQ(std::vector<std::byte>(first_batch.payload.begin(), first_batch.payload.end()),
            first_batch_copy);
  EXPECT_NE(first_batch.payload.data(), second_batch.payload.data());

  {
    auto partial_transport = sitos::fence_test::MakeTransport();
    auto partial_cache_result = sitos::fence_test::OpenAttachedCache(partial_transport);
    ASSERT_TRUE(partial_cache_result.IsOk());
    auto partial_cache = std::move(partial_cache_result).Value();
    ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        partial_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
    sitos::param_cache_test_access::ParamCacheTestAccess::SetMutationHook(
        partial_cache, [](std::size_t count) {
          if (count == 1) throw std::runtime_error("injected partial cache batch failure");
        });

    std::vector<std::byte> partial_batch_storage;
    EXPECT_NO_THROW(
        partial_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
            "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 1, partial_batch_storage,
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
    ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        pre_effect_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
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
    ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
        failure_cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

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
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

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
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ThrowCacheDispatchOnce(cache));
  EXPECT_NO_THROW(transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
      "sitos/session/s1/injected-dispatch-failure", sitos::fence_test::kPublisherA, 1)));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::CacheContains(cache, "injected-dispatch-failure"));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

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
  std::vector<std::byte> batch_storage;
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCacheBatch(
      "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 2, batch_storage,
      {"two", "three"}));
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

// --------------------------------------------------------------------------
// Issue #99 public ParamCache::WaitForLocalDelivery
// --------------------------------------------------------------------------

using sitos::fence_test_access::FenceTestAccess;

// Loops a submitted cache marker back to the subscriber, optionally after a delay,
// so a synchronous in-process completion can be observed by the waiter.
void LoopBackMarker(
    const std::shared_ptr<sitos::fence_test::DeterministicFenceTransport>& transport,
    std::chrono::milliseconds delay = std::chrono::milliseconds{0}) {
  // Captured weakly: the transport owns this observer, so a strong capture would
  // create a reference cycle and leak the transport.
  std::weak_ptr<sitos::fence_test::DeterministicFenceTransport> weak = transport;
  transport->SetPutObserver(
      [weak, delay](const sitos::fence_test::DeterministicFenceTransport::PutRecord& record) {
        if (record.encoding.id != sitos::Encoding::kSitosV1Fence) return;
        if (!record.options.ack_token.has_value()) return;
        const auto route = FenceTestAccess::ParseMarkerRoute(record.key);
        if (!route.has_value()) return;
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const auto owner = weak.lock();
        if (!owner) return;
        owner->Deliver(FenceTestAccess::MakeCacheMarkerSample(
            sitos::fence_test::kPrefix, sitos::fence_test::kSid,
            sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA,
            route->through_sequence,
            FenceTestAccess::ClassifyAttachment(
                sitos::EncodeAckAttachment(*record.options.ack_token))));
      });
}

TEST(FenceParamCacheTest, PublicWaitCoversPriorWritesAndExcludesLaterWrites) {
  auto transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

  // An empty prefix is valid and needs no covered data.
  LoopBackMarker(transport);
  EXPECT_TRUE(cache.WaitForLocalDelivery(sitos::fence_test::kDeadline).IsOk());

  // One Put and one non-empty PutBatch each consume one covered sequence.
  ASSERT_TRUE(cache.Put("covered-put", std::int64_t{1}).IsOk());
  ASSERT_TRUE(cache
                  .PutBatch(std::vector<sitos::BatchEntry>{
                      {"covered-batch-one", sitos::ParamValue(std::int64_t{1})},
                      {"covered-batch-two", sitos::ParamValue(std::int64_t{1})}})
                  .IsOk());
  transport->Deliver(FenceTestAccess::MakeCoveredCachePut("sitos/session/s1/covered-put",
                                                          sitos::fence_test::kPublisherA, 1));
  std::vector<std::byte> batch_storage;
  transport->Deliver(FenceTestAccess::MakeCoveredCacheBatch(
      "sitos/session/s1/:batch", sitos::fence_test::kPublisherA, 2, batch_storage,
      {"covered-batch-one", "covered-batch-two"}));

  const auto markers_before = transport->MarkerCount();
  const auto wait = cache.WaitForLocalDelivery(sitos::fence_test::kDeadline);
  ASSERT_TRUE(wait.IsOk()) << wait.Message();
  EXPECT_EQ(transport->MarkerCount(), markers_before + 1) << "exactly one marker per wait";
  EXPECT_EQ(FenceTestAccess::CacheCompletedThrough(cache), 2U);

  // A write admitted after the linearization point is excluded from that Fence.
  ASSERT_TRUE(cache.Put("later-write", std::int64_t{1}).IsOk());
  EXPECT_EQ(FenceTestAccess::CacheCompletedThrough(cache), 2U)
      << "the later write is not part of the completed prefix";

  // An empty PutBatch consumes no sequence.
  ASSERT_TRUE(cache.PutBatch({}).IsOk());
  transport->Deliver(FenceTestAccess::MakeCoveredCachePut("sitos/session/s1/later-write",
                                                          sitos::fence_test::kPublisherA, 3));
  EXPECT_TRUE(cache.WaitForLocalDelivery(sitos::fence_test::kDeadline).IsOk());
  EXPECT_EQ(FenceTestAccess::CacheCompletedThrough(cache), 3U);

  // Control data never becomes cache content.
  EXPECT_FALSE(FenceTestAccess::CacheContains(cache, ":batch"));
  EXPECT_TRUE(FenceTestAccess::CacheContains(cache, "covered-put"));
}

TEST(FenceParamCacheTest, PublicWaitMapsValidationTimeoutAndReceiverFailure) {
  {  // Definite local validation rejection submits no marker.
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    for (const auto timeout : {std::chrono::milliseconds{0}, std::chrono::milliseconds{-1}}) {
      const auto result = cache.WaitForLocalDelivery(timeout);
      ASSERT_FALSE(result.IsOk());
      EXPECT_EQ(result.StatusCode(), sitos::Status::InvalidArgument);
    }
    EXPECT_EQ(transport->MarkerCount(), 0U);
  }

  {  // An oversized positive deadline saturates instead of wrapping into a false timeout.
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
        cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
    LoopBackMarker(transport);
    EXPECT_TRUE(cache.WaitForLocalDelivery(std::chrono::milliseconds::max()).IsOk());
  }

  {  // No completion by the deadline is client-side Timeout.
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
        cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
    const auto result = cache.WaitForLocalDelivery(std::chrono::milliseconds{20});
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.StatusCode(), sitos::Status::Timeout);
    EXPECT_EQ(transport->MarkerCount(), 1U) << "the marker is never resubmitted";
  }

  {  // A completion that linearizes after the deadline, while the marker Put is still
     // executing, must not later be reported as success (shared-primitive correction).
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
        cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
    LoopBackMarker(transport, std::chrono::milliseconds{60});
    const auto result = cache.WaitForLocalDelivery(std::chrono::milliseconds{20});
    ASSERT_FALSE(result.IsOk()) << "late synchronous completion must not win the deadline race";
    EXPECT_EQ(result.StatusCode(), sitos::Status::Timeout);
  }

  {  // A receiver-side failure preserves its exact status through the public API.
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
        cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
    ASSERT_TRUE(FenceTestAccess::ThrowCacheDispatchOnce(cache));
    EXPECT_NO_THROW(transport->Deliver(FenceTestAccess::MakeCoveredCachePut(
        "sitos/session/s1/receiver-failure", sitos::fence_test::kPublisherA, 1)));
    LoopBackMarker(transport);
    const auto result = cache.WaitForLocalDelivery(sitos::fence_test::kDeadline);
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.StatusCode(), sitos::Status::OutcomeUnknown);
  }

  {  // A detached cache rejects the wait before submitting anything.
    auto transport = sitos::fence_test::MakeTransport();
    auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
    ASSERT_TRUE(cache_result.IsOk());
    auto cache = std::move(cache_result).Value();
    cache.Detach();
    const auto result = cache.WaitForLocalDelivery(sitos::fence_test::kDeadline);
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.StatusCode(), sitos::Status::InvalidArgument);
    EXPECT_EQ(transport->MarkerCount(), 0U);
  }
}

TEST(FenceParamCacheTest, PublicWaitRejectsSecondPendingWaitWithoutCorruptingFirst) {
  auto transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

  // The gated marker Put returns without completing, so the first wait stays pending.
  LoopBackMarker(transport);
  transport->GateMarkerCompletion();
  sitos::Result<void> first = sitos::Result<void>::Err(sitos::Status::Error, "unset");
  std::thread waiter([&] { first = cache.WaitForLocalDelivery(std::chrono::seconds{5}); });
  while (transport->MarkerCount() == 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});

  const auto second = cache.WaitForLocalDelivery(sitos::fence_test::kDeadline);
  ASSERT_FALSE(second.IsOk());
  EXPECT_EQ(second.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(second.Error(), std::make_error_code(std::errc::operation_in_progress));
  EXPECT_EQ(transport->MarkerCount(), 1U) << "the rejected wait emits no second marker";

  transport->ReleaseMarkerCompletion();  // replays the gated marker through the observer
  waiter.join();
  EXPECT_TRUE(first.IsOk()) << "the first waiter is unaffected: " << first.Message();
}

}  // namespace
