// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "fence_test_support.hpp"
#include "storage_node_test_access.hpp"

namespace {

enum class BufferEngineMode { Healthy, ReturnFalse, ThrowOnGet, ThrowOnPut };

class ControlledBufferEngine final : public sitos::InMemoryEngine {
 public:
  ControlledBufferEngine(std::shared_ptr<BufferEngineMode> mode,
                         std::shared_ptr<std::size_t> put_calls)
      : mode_(std::move(mode)), put_calls_(std::move(put_calls)) {}

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    if (*mode_ == BufferEngineMode::ThrowOnGet) {
      throw std::runtime_error("injected durable buffer read failure");
    }
    return InMemoryEngine::Get(key, sink);
  }

  bool Put(std::string_view key, sitos::Bytes value) override {
    ++*put_calls_;
    if (*mode_ == BufferEngineMode::ReturnFalse) return false;
    if (*mode_ == BufferEngineMode::ThrowOnPut) {
      throw std::runtime_error("injected durable buffer write failure");
    }
    return InMemoryEngine::Put(key, value);
  }

 private:
  std::shared_ptr<BufferEngineMode> mode_;
  std::shared_ptr<std::size_t> put_calls_;
};

void ExpectRemoteFence(const sitos::AckResultV1& result, sitos::Status status,
                       sitos::AckDurability durability, std::uint64_t through,
                       std::uint64_t failed) {
  EXPECT_EQ(result.operation_kind, sitos::AckOperationKind::Fence);
  EXPECT_EQ(result.status, status);
  EXPECT_EQ(result.durability, durability);
  EXPECT_EQ(result.applied_count, 0U);
  EXPECT_EQ(result.failed_index, sitos::kAckNoFailedIndex);
  EXPECT_EQ(result.through_sequence, through);
  EXPECT_EQ(result.failed_sequence, failed);
}

TEST(FenceStorageNodeTest, DispatchesFenceAndBindsTheSessionGeneration) {
  auto transport = sitos::fence_test::MakeTransport();
  auto node = sitos::fence_test::StartNode(transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      node->CreateSession(sitos::fence_test::kSid, sitos::fence_test::DurableSessionOptions())
          .IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, sitos::fence_test::kSid, sitos::fence_test::kSessionGeneration));

  auto pre_effect_publisher = sitos::fence_test::kPublisherB;
  pre_effect_publisher[0] = std::byte{0x71};
  bool pre_effect_observer_called = false;
  ASSERT_TRUE(sitos::storage_node_test_access::StorageNodeTestAccess::SetSubscriberEntryObserver(
      *node, [&pre_effect_observer_called] {
        pre_effect_observer_called = true;
        throw std::bad_alloc();
      }));
  EXPECT_NO_THROW(
      transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
          "sitos/buffers/s1/durable/pre-effect", pre_effect_publisher, 1)));
  EXPECT_TRUE(pre_effect_observer_called);
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::BufferValueExists(*node, "s1", "pre-effect"));
  ASSERT_TRUE(sitos::storage_node_test_access::StorageNodeTestAccess::SetSubscriberEntryObserver(
      *node, {}));
  const auto pre_effect_token = sitos::fence_test::Token(std::byte{0x3f});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      pre_effect_publisher, sitos::AckDurability::Applied, 1, pre_effect_token));
  const auto pre_effect =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, pre_effect_token);
  ASSERT_TRUE(pre_effect.has_value());
  ExpectRemoteFence(*pre_effect, sitos::Status::Error, sitos::AckDurability::Applied, 1, 1);

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ThrowStorageDispatchOnce(*node));
  EXPECT_NO_THROW(
      transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
          "sitos/buffers/s1/durable/injected-dispatch-failure", sitos::fence_test::kPublisherB,
          2)));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::BufferValueExists(
      *node, "s1", "injected-dispatch-failure"));
  const auto dispatch_failure_token = sitos::fence_test::Token(std::byte{0x40});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Applied, 1, dispatch_failure_token));
  const auto dispatch_failure =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, dispatch_failure_token);
  ASSERT_TRUE(dispatch_failure.has_value());
  ExpectRemoteFence(*dispatch_failure, sitos::Status::OutcomeUnknown, sitos::AckDurability::Applied,
                    1, 1);

  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/s1/durable/value", sitos::fence_test::kPublisherA, 1));
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::BufferValueExists(*node, "s1", "value"));

  const auto applied_token = sitos::fence_test::Token(std::byte{0x41});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, applied_token));
  const auto applied =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, applied_token);
  ASSERT_TRUE(applied.has_value());
  ExpectRemoteFence(*applied, sitos::Status::Ok, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceTokenClaims(*node, applied_token), 1U);

  // Duplicate and future sequences are rejected before durable mutation.
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/s1/durable/duplicate", sitos::fence_test::kPublisherA, 1));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/s1/durable/future", sitos::fence_test::kPublisherA, 3));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::BufferValueExists(*node, "s1", "duplicate"));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::BufferValueExists(*node, "s1", "future"));

  // Same fingerprint returns the immutable result without repeat buffer mutation;
  // a different fingerprint is rejected without replacing it.
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, applied_token));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceTokenClaims(*node, applied_token), 1U);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node), 1U);
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 2, applied_token));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, applied_token),
            applied);

  const auto stale_token = sitos::fence_test::Token(std::byte{0x42});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kPublisherB, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, stale_token));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, stale_token));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceTokenClaims(*node, stale_token), 0U);

  const auto missing_token = sitos::fence_test::Token(std::byte{0x43});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "missing", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, missing_token));
  const auto missing =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, missing_token);
  ASSERT_TRUE(missing.has_value());
  ExpectRemoteFence(*missing, sitos::Status::NotFound, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);

  // Marker validity wins over Session lookup: a recoverable malformed marker
  // always retains Error even when its Session is absent.
  const auto malformed_missing_token = sitos::fence_test::Token(std::byte{0x4a});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMalformedBufferMarker(
      "sitos", "missing", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, malformed_missing_token));
  const auto malformed_missing =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, malformed_missing_token);
  ASSERT_TRUE(malformed_missing.has_value());
  ExpectRemoteFence(*malformed_missing, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);

  const auto malformed_token = sitos::fence_test::Token(std::byte{0x44});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMalformedBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, malformed_token));
  const auto malformed =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, malformed_token);
  ASSERT_TRUE(malformed.has_value());
  ExpectRemoteFence(*malformed, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);
  const auto malformed_route_token = sitos::fence_test::Token(std::byte{0x48});
  const std::string malformed_route =
      "sitos/meta/fence/buffer/s1/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kSessionGeneration) + "/durable/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kPublisherA) + "/applied/01";
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMarkerFromRoute(
      malformed_route, malformed_route_token));
  const auto malformed_route_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, malformed_route_token);
  ASSERT_TRUE(malformed_route_result.has_value());
  ExpectRemoteFence(*malformed_route_result, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);

  const auto trailing_slash_token = sitos::fence_test::Token(std::byte{0x4d});
  const std::string trailing_slash_route =
      "sitos/meta/fence/buffer/s1/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kSessionGeneration) + "/durable/" +
      sitos::fence_internal::FormatFenceUuid(sitos::fence_test::kPublisherA) + "/applied/1/";
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMarkerFromRoute(
      trailing_slash_route, trailing_slash_token));
  const auto trailing_slash_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, trailing_slash_token);
  ASSERT_TRUE(trailing_slash_result.has_value());
  ExpectRemoteFence(*trailing_slash_result, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);

  const auto unrecoverable_token = sitos::fence_test::Token(std::byte{0x49});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeMarkerFromRoute(
      "sitos/meta/fence/buffer/s1/not-a-uuid/durable/not-a-publisher/applied/1",
      unrecoverable_token));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, unrecoverable_token));

  // Once a marker token is claimed, every exception path must retain one
  // immutable terminal result rather than leaking an unbounded Processing entry.
  const auto throwing_token = sitos::fence_test::Token(std::byte{0x4e});
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::FailFenceCompletionRetentionOnce(*node));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ThrowFenceAfterTokenClaimOnce(*node));
  const auto throwing_marker = sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, throwing_token);
  transport->Deliver(throwing_marker);
  const auto throwing_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, throwing_token);
  ASSERT_TRUE(throwing_result.has_value());
  ExpectRemoteFence(*throwing_result, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::AckRegistryProcessingEntries(*node), 0U);
  transport->Deliver(throwing_marker);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, throwing_token),
            throwing_result);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceTokenClaims(*node, throwing_token), 1U);

  const auto entries_before_missing_token =
      sitos::fence_test_access::FenceTestAccess::AckRegistryEntries(*node);
  const auto no_token_marker =
      sitos::fence_test_access::FenceTestAccess::MakeMalformedBufferMarkerWithoutAck(
          "sitos", "s1", sitos::fence_test::kSessionGeneration);
  transport->Deliver(no_token_marker);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::AckRegistryEntries(*node),
            entries_before_missing_token);

  // A real internal buffer Publisher reuses the ADR-0028 query loop rather than
  // completing a local waiter.
  ASSERT_TRUE(node->CreateSession("s2", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "s2", sitos::fence_test::kAttachGeneration));
  transport->SetPutObserver([&transport](const auto& record) {
    transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeSampleFromPut(
        record.key, record.payload, record.encoding, record.options));
  });
  auto buffer_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::BufferReceiverBinding(
          "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
          sitos::AckDurability::Applied));
  ASSERT_TRUE(buffer_publisher.SubmitPush().IsOk());
  auto remote_fence = buffer_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(remote_fence.IsOk());
  transport->GateAckQueryReturn();
  auto remote_wait =
      std::async(std::launch::async, [&] { return buffer_publisher.Wait(remote_fence.Value()); });
  transport->WaitForAckQueryObservation();
  auto close_buffer_publisher = std::async(std::launch::async, [&] { buffer_publisher.Close(); });
  EXPECT_TRUE(sitos::fence_test::WaitUntil([&] { return !buffer_publisher.AcceptingOperations(); },
                                           std::chrono::seconds(1)))
      << "Close must stop accepting operations within the test deadline";
  EXPECT_EQ(close_buffer_publisher.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  transport->ReleaseAckQueryReturn();
  const auto remote_result = remote_wait.get();
  close_buffer_publisher.get();
  ASSERT_TRUE(remote_result.IsOk());
  ExpectRemoteFence(remote_result.Value(), sitos::Status::Ok, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);
  EXPECT_GT(transport->AckQueryCount(), 0U);
  const auto queries_after_completion = transport->AckQueryCount();
  const auto immutable_remote_result = buffer_publisher.Wait(remote_fence.Value());
  ASSERT_TRUE(immutable_remote_result.IsOk());
  EXPECT_EQ(immutable_remote_result.Value(), remote_result.Value());
  EXPECT_EQ(transport->AckQueryCount(), queries_after_completion);

  // Buffer ACK decoding uses the same hidden terminal-result generation
  // linearization as direct cache completion. Replace the generation after
  // Wait's entry check and the callback's first sample; its final sample must
  // still downgrade the result before it becomes observable.
  auto buffer_completion_race_uuid = sitos::fence_test::kPublisherA;
  buffer_completion_race_uuid[15] ^= std::byte{0x04};
  auto buffer_completion_race_publisher =
      sitos::fence_test_access::FenceTestAccess::CreatePublisher(
          *transport, buffer_completion_race_uuid,
          sitos::fence_test_access::FenceTestAccess::BufferReceiverBinding(
              "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
              sitos::AckDurability::Applied));
  ASSERT_TRUE(buffer_completion_race_publisher.SubmitPush().IsOk());
  auto buffer_completion_race =
      buffer_completion_race_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(buffer_completion_race.IsOk());
  transport->ReplaceGenerationAfterReads(2);
  const auto buffer_completion_race_result =
      buffer_completion_race_publisher.Wait(buffer_completion_race.Value());
  ASSERT_TRUE(buffer_completion_race_result.IsOk());
  EXPECT_EQ(buffer_completion_race_result.Value().status, sitos::Status::Disconnected);
  const auto data_submissions_after_buffer_replacement = transport->DataSubmissionCount();
  EXPECT_EQ(buffer_completion_race_publisher.SubmitPush().StatusCode(),
            sitos::Status::Disconnected);
  EXPECT_EQ(transport->DataSubmissionCount(), data_submissions_after_buffer_replacement);

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetFenceDurabilityBarrier(
      *node, [](sitos::StorageEngine&) { return sitos::Result<void>::Ok(); }));
  const auto empty_synced_token = sitos::fence_test::Token(std::byte{0x51});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Synced, 0, empty_synced_token));
  const auto empty_synced =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, empty_synced_token);
  ASSERT_TRUE(empty_synced.has_value());
  ExpectRemoteFence(*empty_synced, sitos::Status::Ok, sitos::AckDurability::Synced, 0,
                    sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceDurabilityBarrierCalls(*node), 0U)
      << "an empty synchronized prefix validates capability but does not invoke the barrier";

  const auto synced_token = sitos::fence_test::Token(std::byte{0x46});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Synced, 1, synced_token));
  const auto synced_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, synced_token);
  ASSERT_TRUE(synced_result.has_value());
  ExpectRemoteFence(*synced_result, sitos::Status::Ok, sitos::AckDurability::Synced, 1,
                    sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceDurabilityBarrierCalls(*node), 1U);
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Synced, 1, synced_token));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceDurabilityBarrierCalls(*node), 1U);

  // A synchronized marker overtaken by already processed sequence 2 must fail
  // before invoking the durability barrier for through_sequence 1.
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/s2/durable/overtake-1", sitos::fence_test::kPublisherA, 1));
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/s2/durable/overtake-2", sitos::fence_test::kPublisherA, 2));
  const auto synced_overtake_token = sitos::fence_test::Token(std::byte{0x50});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Synced, 1, synced_overtake_token));
  const auto synced_overtake =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, synced_overtake_token);
  ASSERT_TRUE(synced_overtake.has_value());
  ExpectRemoteFence(*synced_overtake, sitos::Status::OutcomeUnknown, sitos::AckDurability::Synced,
                    1, sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceDurabilityBarrierCalls(*node), 1U);

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetFenceDurabilityBarrier(
      *node, [](sitos::StorageEngine&) {
        return sitos::Result<void>::Err(sitos::Status::Error, "barrier failed");
      }));
  const auto barrier_error_token = sitos::fence_test::Token(std::byte{0x47});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Synced, 1, barrier_error_token));
  const auto barrier_error =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, barrier_error_token);
  ASSERT_TRUE(barrier_error.has_value());
  ExpectRemoteFence(*barrier_error, sitos::Status::Error, sitos::AckDurability::Synced, 1,
                    sitos::kAckNoFailedSequence);

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetFenceDurabilityBarrier(
      *node, [](sitos::StorageEngine&) -> sitos::Result<void> {
        throw std::runtime_error("injected durability uncertainty");
      }));
  const auto barrier_throw_token = sitos::fence_test::Token(std::byte{0x4f});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Synced, 1, barrier_throw_token));
  const auto barrier_throw =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, barrier_throw_token);
  ASSERT_TRUE(barrier_throw.has_value());
  ExpectRemoteFence(*barrier_throw, sitos::Status::OutcomeUnknown, sitos::AckDurability::Synced, 1,
                    sitos::kAckNoFailedSequence);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::AckRegistryProcessingEntries(*node), 0U);

  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetFenceDurabilityBarrier(*node, {}));
  const auto unsupported_empty_synced_token = sitos::fence_test::Token(std::byte{0x52});
  auto unsupported_empty_publisher = sitos::fence_test::kPublisherA;
  unsupported_empty_publisher[15] ^= std::byte{0x02};
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s2", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Durable,
      unsupported_empty_publisher, sitos::AckDurability::Synced, 0,
      unsupported_empty_synced_token));
  const auto unsupported_empty_synced = sitos::fence_test_access::FenceTestAccess::FindAckResult(
      *node, unsupported_empty_synced_token);
  ASSERT_TRUE(unsupported_empty_synced.has_value());
  ExpectRemoteFence(*unsupported_empty_synced, sitos::Status::InvalidArgument,
                    sitos::AckDurability::Synced, 0, sitos::kAckNoFailedSequence);

  // Empty-prefix Fence cannot succeed for a buffer capability disabled by the
  // active Session, regardless of whether a lane has been observed.
  ASSERT_TRUE(node->CreateSession("disabled", sitos::SessionOptions{}).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "disabled", sitos::fence_test::kSessionGeneration));
  const auto disabled_durable_token = sitos::fence_test::Token(std::byte{0x4b});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "disabled", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, disabled_durable_token));
  const auto disabled_durable =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, disabled_durable_token);
  ASSERT_TRUE(disabled_durable.has_value());
  ExpectRemoteFence(*disabled_durable, sitos::Status::InvalidArgument,
                    sitos::AckDurability::Applied, 0, sitos::kAckNoFailedSequence);

  const auto disabled_ephemeral_token = sitos::fence_test::Token(std::byte{0x4c});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "disabled", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Ephemeral,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, disabled_ephemeral_token));
  const auto disabled_ephemeral =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, disabled_ephemeral_token);
  ASSERT_TRUE(disabled_ephemeral.has_value());
  ExpectRemoteFence(*disabled_ephemeral, sitos::Status::InvalidArgument,
                    sitos::AckDurability::Applied, 0, sitos::kAckNoFailedSequence);

  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/disabled/durable/rejected", sitos::fence_test::kPublisherA, 1));
  const auto disabled_data_token = sitos::fence_test::Token(std::byte{0x53});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "disabled", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, disabled_data_token));
  const auto disabled_data =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, disabled_data_token);
  ASSERT_TRUE(disabled_data.has_value());
  {
    SCOPED_TRACE("disabled covered data");
    ExpectRemoteFence(*disabled_data, sitos::Status::InvalidArgument, sitos::AckDurability::Applied,
                      1, 1);
  }

  // Closing-first is a valid marker path: it retains a Fence-kind InvalidArgument
  // result rather than returning an outer local error or claiming a new lane.
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::BeginClose(*node, "s1"));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::CloseSessionFenceDispatch(*node, "s1"));
  const auto closing_token = sitos::fence_test::Token(std::byte{0x45});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, closing_token));
  const auto closing =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, closing_token);
  ASSERT_TRUE(closing.has_value());
  ExpectRemoteFence(*closing, sitos::Status::InvalidArgument, sitos::AckDurability::Applied, 1,
                    sitos::kAckNoFailedSequence);

  const auto before_synced = transport->MarkerSubmissionCount();
  const auto applications_before_synced =
      sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node);
  const auto synced = sitos::fence_test_access::FenceTestAccess::SubmitProductionSyncedFence(
      *transport, "s1", sitos::fence_test::kSessionGeneration, sitos::fence_test::kPublisherA, 1);
  EXPECT_EQ(synced.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport->MarkerSubmissionCount(), before_synced);
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node),
            applications_before_synced);

  auto engine_mode = std::make_shared<BufferEngineMode>(BufferEngineMode::Healthy);
  auto put_calls = std::make_shared<std::size_t>(0);
  auto failing_transport = sitos::fence_test::MakeTransport();
  auto failing_node = sitos::fence_test::StartNode(
      failing_transport, [engine_mode, put_calls](std::string_view) {
        return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
            std::make_unique<ControlledBufferEngine>(engine_mode, put_calls));
      });
  ASSERT_NE(failing_node, nullptr);
  ASSERT_TRUE(
      failing_node->CreateSession("failures", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *failing_node, "failures", sitos::fence_test::kSessionGeneration));

  *engine_mode = BufferEngineMode::ReturnFalse;
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/failures/durable/false", sitos::fence_test::kPublisherA, 1));
  const auto false_token = sitos::fence_test::Token(std::byte{0x54});
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "failures", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, false_token));
  const auto false_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*failing_node, false_token);
  ASSERT_TRUE(false_result.has_value());
  ExpectRemoteFence(*false_result, sitos::Status::OutcomeUnknown, sitos::AckDurability::Applied, 1,
                    1);

  *engine_mode = BufferEngineMode::ThrowOnPut;
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/failures/durable/throw", sitos::fence_test::kPublisherB, 1));
  const auto throw_token = sitos::fence_test::Token(std::byte{0x55});
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "failures", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherB, sitos::AckDurability::Applied, 1, throw_token));
  const auto throw_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*failing_node, throw_token);
  ASSERT_TRUE(throw_result.has_value());
  ExpectRemoteFence(*throw_result, sitos::Status::OutcomeUnknown, sitos::AckDurability::Applied, 1,
                    1);

  auto read_failure_publisher = sitos::fence_test::kPublisherA;
  read_failure_publisher[15] ^= std::byte{0x02};
  *engine_mode = BufferEngineMode::ThrowOnGet;
  const auto put_calls_before_read_failure = *put_calls;
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/failures/durable/read-failure", read_failure_publisher, 1));
  EXPECT_EQ(*put_calls, put_calls_before_read_failure)
      << "a pre-effect durable read failure must not invoke Put";
  const auto read_failure_token = sitos::fence_test::Token(std::byte{0x57});
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "failures", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      read_failure_publisher, sitos::AckDurability::Applied, 1, read_failure_token));
  const auto read_failure_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*failing_node, read_failure_token);
  ASSERT_TRUE(read_failure_result.has_value());
  ExpectRemoteFence(*read_failure_result, sitos::Status::Error, sitos::AckDurability::Applied, 1,
                    1);
  *engine_mode = BufferEngineMode::Healthy;
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::BufferValueExists(
      *failing_node, "failures", "read-failure"));

  auto conflict_publisher = sitos::fence_test::kPublisherA;
  conflict_publisher[15] ^= std::byte{0x01};
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/failures/durable/conflict", conflict_publisher, 1));
  const std::array<std::byte, 1> conflicting_payload{std::byte{0x02}};
  failing_transport->Deliver(sitos::TransportSample{
      "sitos/buffers/failures/durable/conflict", conflicting_payload,
      sitos::Encoding{"zenoh/bytes"}, sitos::AckAttachmentAbsent{},
      sitos::TransportSample::Kind::Put, sitos::FenceLaneMetadata{conflict_publisher, 2}});
  const auto conflict_token = sitos::fence_test::Token(std::byte{0x56});
  failing_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "failures", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      conflict_publisher, sitos::AckDurability::Applied, 2, conflict_token));
  const auto conflict_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*failing_node, conflict_token);
  ASSERT_TRUE(conflict_result.has_value());
  {
    SCOPED_TRACE("durable write-once conflict");
    ExpectRemoteFence(*conflict_result, sitos::Status::InvalidArgument,
                      sitos::AckDurability::Applied, 2, 2);
  }
}

}  // namespace
