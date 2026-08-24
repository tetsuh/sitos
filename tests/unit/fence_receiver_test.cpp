// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "fence_test_support.hpp"

namespace {

sitos::FenceUuid PublisherForIndex(std::uint32_t index) {
  auto uuid = sitos::fence_test::kPublisherA;
  uuid[12] = static_cast<std::byte>((index >> 24U) & 0xffU);
  uuid[13] = static_cast<std::byte>((index >> 16U) & 0xffU);
  uuid[14] = static_cast<std::byte>((index >> 8U) & 0xffU);
  uuid[15] = static_cast<std::byte>(index & 0xffU);
  return uuid;
}

void ExpectFenceResult(const sitos::AckResultV1& result, sitos::Status status,
                       std::uint64_t through, std::uint64_t failed) {
  EXPECT_EQ(result.operation_kind, sitos::AckOperationKind::Fence);
  EXPECT_EQ(result.status, status);
  EXPECT_EQ(result.applied_count, 0U);
  EXPECT_EQ(result.failed_index, sitos::kAckNoFailedIndex);
  EXPECT_EQ(result.through_sequence, through);
  EXPECT_EQ(result.failed_sequence, failed);
}

TEST(FenceReceiverTest, EvaluatesPrefixesFailuresBoundsAndCapacityPoison) {
  auto receiver = sitos::fence_test_access::FenceTestAccess::CreateReceiver();

  ASSERT_TRUE(receiver.ObserveExpected(1).IsOk());
  ASSERT_TRUE(receiver.ObserveFuture(3).IsOk());
  // Sequence 3 crossed the marker boundary even when the marker asks only for 1.
  ExpectFenceResult(receiver.EvaluateMarker(1), sitos::Status::OutcomeUnknown, 1,
                    sitos::kAckNoFailedSequence);
  // First failure (missing 2) wins over the later stale duplicate and remains
  // selectable for every through value that includes it.
  ExpectFenceResult(receiver.EvaluateMarker(2), sitos::Status::OutcomeUnknown, 2, 2);
  ASSERT_TRUE(receiver.ObserveStale(2).IsOk());
  ExpectFenceResult(receiver.EvaluateMarker(3), sitos::Status::OutcomeUnknown, 3, 2);

  receiver.Reset();
  ASSERT_TRUE(receiver.ObserveExpected(1).IsOk());
  ASSERT_TRUE(receiver.ObserveStale(1).IsOk());
  ExpectFenceResult(receiver.EvaluateMarker(1), sitos::Status::Error, 1, 1);

  receiver.Reset();
  ASSERT_TRUE(
      receiver.ObserveMalformed({.publisher_uuid = sitos::fence_test::kPublisherA, .sequence = 2})
          .IsOk());
  ExpectFenceResult(receiver.EvaluateMarker(1), sitos::Status::OutcomeUnknown, 1,
                    sitos::kAckNoFailedSequence);
  ExpectFenceResult(receiver.EvaluateMarker(2), sitos::Status::Error, 2, 2);
  receiver.Reset();
  ASSERT_TRUE(receiver
                  .ObserveMalformed(
                      {.publisher_uuid = sitos::fence_test::kPublisherA, .sequence = std::nullopt})
                  .IsOk());
  ExpectFenceResult(receiver.EvaluateMarker(1), sitos::Status::Error, 1,
                    sitos::kAckNoFailedSequence);
  receiver.Reset();
  ASSERT_TRUE(
      receiver.ObserveMalformed({.publisher_uuid = std::nullopt, .sequence = std::nullopt}).IsOk());
  ExpectFenceResult(receiver.EvaluateMarker(1), sitos::Status::OutcomeUnknown, 1, 1);

  const auto consecutive =
      sitos::fence_test_access::FenceTestAccess::ExerciseConsecutiveReservations();
  EXPECT_TRUE(consecutive.first_admitted);
  EXPECT_TRUE(consecutive.second_admitted);
  ExpectFenceResult(consecutive.through_two, sitos::Status::Ok, 2, sitos::kAckNoFailedSequence);

  const auto production_dispatch =
      sitos::fence_test_access::FenceTestAccess::ExerciseGlobalDispatchCapacity(256);
  EXPECT_EQ(production_dispatch.admitted, 256U);
  EXPECT_TRUE(production_dispatch.overflow_rejected);
  ASSERT_EQ(production_dispatch.callback_tickets.size(), 256U);
  for (std::size_t index = 0; index < production_dispatch.callback_tickets.size(); ++index) {
    EXPECT_EQ(production_dispatch.callback_tickets[index], index);
  }

  sitos::fence_internal::FenceReceiverRegistry combined_registry(4096);
  for (std::uint32_t index = 0; index < 4096; ++index) {
    ASSERT_TRUE(combined_registry.RecordBufferObservation(
        "combined", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
        PublisherForIndex(index), 1));
  }
  const auto combined_sequence_one = PublisherForIndex(4096);
  const auto combined_future = PublisherForIndex(4097);
  const auto combined_uuid_only = PublisherForIndex(4098);
  const auto combined_sequence_one_capacity =
      sitos::fence_test_access::FenceTestAccess::ExerciseGlobalDispatchCapacity(256, [&] {
        EXPECT_FALSE(combined_registry.RecordBufferOverflow(
            "combined-sequence-one", sitos::fence_test::kSessionGeneration,
            sitos::BufferClass::Durable, combined_sequence_one, 1));
      });
  const auto combined_future_capacity =
      sitos::fence_test_access::FenceTestAccess::ExerciseGlobalDispatchCapacity(256, [&] {
        EXPECT_FALSE(combined_registry.RecordBufferOverflow(
            "combined-future", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
            combined_future, 7));
      });
  const auto combined_uuid_only_capacity =
      sitos::fence_test_access::FenceTestAccess::ExerciseGlobalDispatchCapacity(256, [&] {
        EXPECT_FALSE(combined_registry.RecordMalformedBuffer(
            "combined-uuid-only", sitos::fence_test::kSessionGeneration,
            sitos::BufferClass::Durable,
            {.publisher_uuid = combined_uuid_only, .sequence = std::nullopt}));
      });
  for (const auto& capacity :
       {combined_sequence_one_capacity, combined_future_capacity, combined_uuid_only_capacity}) {
    EXPECT_EQ(capacity.admitted, 256U);
    EXPECT_TRUE(capacity.overflow_rejected);
  }
  for (const auto& row : std::array{
           std::pair{"combined-sequence-one", combined_sequence_one},
           std::pair{"combined-future", combined_future},
           std::pair{"combined-uuid-only", combined_uuid_only},
       }) {
    SCOPED_TRACE(row.first);
    ExpectFenceResult(
        combined_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                         sitos::BufferClass::Durable, row.second,
                                         sitos::AckDurability::Applied, 0),
        sitos::Status::OutcomeUnknown, 0, sitos::kAckNoFailedSequence);
    ExpectFenceResult(
        combined_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                         sitos::BufferClass::Durable, row.second,
                                         sitos::AckDurability::Applied, 3),
        sitos::Status::OutcomeUnknown, 3, 1);
    ExpectFenceResult(
        combined_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                         sitos::BufferClass::Durable, row.second,
                                         sitos::AckDurability::Applied, 7),
        sitos::Status::OutcomeUnknown, 7, 1);
    ExpectFenceResult(
        combined_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                         sitos::BufferClass::Ephemeral, row.second,
                                         sitos::AckDurability::Applied, 0),
        sitos::Status::Ok, 0, sitos::kAckNoFailedSequence);
    combined_registry.EraseSession(row.first);
    ExpectFenceResult(
        combined_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                         sitos::BufferClass::Durable, row.second,
                                         sitos::AckDurability::Applied, 0),
        sitos::Status::Ok, 0, sitos::kAckNoFailedSequence);
  }

  receiver.Reset();
  for (const auto& row : std::array{
           sitos::fence_test_access::OverflowCase{.kind = "stale",
                                                  .sequence = 1,
                                                  .through = 1,
                                                  .status = sitos::Status::Error,
                                                  .failed = 1},
           sitos::fence_test_access::OverflowCase{.kind = "expected",
                                                  .sequence = 2,
                                                  .through = 2,
                                                  .status = sitos::Status::OutcomeUnknown,
                                                  .failed = 2},
           sitos::fence_test_access::OverflowCase{.kind = "future",
                                                  .sequence = 4,
                                                  .through = 4,
                                                  .status = sitos::Status::OutcomeUnknown,
                                                  .failed = 2},
           sitos::fence_test_access::OverflowCase{.kind = "malformed-sequence",
                                                  .sequence = 5,
                                                  .through = 5,
                                                  .status = sitos::Status::Error,
                                                  .failed = 5},
           sitos::fence_test_access::OverflowCase{.kind = "malformed-uuid-only",
                                                  .sequence = 0,
                                                  .through = 5,
                                                  .status = sitos::Status::Error,
                                                  .failed = sitos::kAckNoFailedSequence},
       }) {
    SCOPED_TRACE(row.kind);
    receiver.ResetRetainedFailureOnly();
    ASSERT_TRUE(receiver.RecordOverflow(row));
    ExpectFenceResult(receiver.EvaluateMarker(row.through), row.status, row.through, row.failed);
    if (row.through > 0) {
      const auto earlier = receiver.EvaluateMarker(row.through - 1);
      if (row.kind == "future") {
        ExpectFenceResult(earlier, sitos::Status::OutcomeUnknown, row.through - 1, row.failed);
      } else if (row.kind == "malformed-uuid-only") {
        ExpectFenceResult(earlier, sitos::Status::Error, row.through - 1,
                          sitos::kAckNoFailedSequence);
      } else {
        ExpectFenceResult(earlier, sitos::Status::OutcomeUnknown, row.through - 1,
                          sitos::kAckNoFailedSequence);
      }
    }
  }

  // Exercise the production registry itself rather than a test-side capacity
  // model. Two retained lanes fill this bounded instance; every unretained new
  // lane rejection poisons only the exact Session-generation/class scope and
  // never invents a rejected sequence.
  sitos::fence_internal::FenceReceiverRegistry bounded_registry(2);
  const auto publisher_b = PublisherForIndex(1);
  ASSERT_TRUE(bounded_registry.RecordBufferObservation(
      sitos::fence_test::kSid, sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, 1));
  ASSERT_TRUE(bounded_registry.RecordBufferObservation(
      sitos::fence_test::kSid, sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      publisher_b, 1));
  ASSERT_EQ(bounded_registry.Size(), 2U);
  const auto registry_sequence_one = PublisherForIndex(2);
  const auto registry_future = PublisherForIndex(3);
  const auto registry_uuid_only = PublisherForIndex(4);
  EXPECT_FALSE(bounded_registry.RecordBufferOverflow(
      "registry-sequence-one", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      registry_sequence_one, 1));
  EXPECT_FALSE(bounded_registry.RecordBufferOverflow(
      "registry-future", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      registry_future, 4));
  EXPECT_FALSE(bounded_registry.RecordMalformedBuffer(
      "registry-uuid-only", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      {.publisher_uuid = registry_uuid_only, .sequence = std::nullopt}));
  for (const auto& row : std::array{
           std::pair{"registry-sequence-one", registry_sequence_one},
           std::pair{"registry-future", registry_future},
           std::pair{"registry-uuid-only", registry_uuid_only},
       }) {
    SCOPED_TRACE(row.first);
    ExpectFenceResult(
        bounded_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                        sitos::BufferClass::Durable, row.second,
                                        sitos::AckDurability::Applied, 0),
        sitos::Status::OutcomeUnknown, 0, sitos::kAckNoFailedSequence);
    ExpectFenceResult(
        bounded_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                        sitos::BufferClass::Durable, row.second,
                                        sitos::AckDurability::Applied, 3),
        sitos::Status::OutcomeUnknown, 3, 1);
    ExpectFenceResult(
        bounded_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                        sitos::BufferClass::Durable, row.second,
                                        sitos::AckDurability::Applied, 4),
        sitos::Status::OutcomeUnknown, 4, 1);
    ExpectFenceResult(
        bounded_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                        sitos::BufferClass::Ephemeral, row.second,
                                        sitos::AckDurability::Applied, 0),
        sitos::Status::Ok, 0, sitos::kAckNoFailedSequence);
    bounded_registry.EraseSession(row.first);
    ExpectFenceResult(
        bounded_registry.EvaluateBuffer(row.first, sitos::fence_test::kSessionGeneration,
                                        sitos::BufferClass::Durable, row.second,
                                        sitos::AckDurability::Applied, 0),
        sitos::Status::Ok, 0, sitos::kAckNoFailedSequence);
  }
  bounded_registry.EraseSession(sitos::fence_test::kSid);
  EXPECT_EQ(bounded_registry.Size(), 0U);

  sitos::fence_internal::FenceLaneState exhausted_lane;
  exhausted_lane.SetCompletedForTesting(UINT64_MAX);
  exhausted_lane.RecordOverflow(UINT64_MAX);
  const auto exhausted_duplicate = exhausted_lane.Evaluate(UINT64_MAX);
  EXPECT_EQ(exhausted_duplicate.status, sitos::Status::Error);

  // Exercise the real StorageNode registry at the exact 4096-lane boundary.
  auto transport = sitos::fence_test::MakeTransport();
  auto node = sitos::fence_test::StartNode(transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->CreateSession("capacity", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "capacity", sitos::fence_test::kSessionGeneration));
  for (std::uint32_t index = 0; index < 4096; ++index) {
    transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
        "sitos/buffers/capacity/ephemeral/value", PublisherForIndex(index), 1));
  }
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node), 4096U);
  const auto rejected_capacity_publisher = PublisherForIndex(4096);
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/capacity/ephemeral/rejected", rejected_capacity_publisher, 1));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node), 4096U);

  const auto poison_token = sitos::fence_test::Token(std::byte{0x71});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "capacity", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Ephemeral,
      rejected_capacity_publisher, sitos::AckDurability::Applied, 0, poison_token));
  const auto poison = sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, poison_token);
  ASSERT_TRUE(poison.has_value());
  ExpectFenceResult(*poison, sitos::Status::OutcomeUnknown, 0, sitos::kAckNoFailedSequence);

  ASSERT_TRUE(node->CloseSession("capacity").IsOk());
  ASSERT_TRUE(node->CreateSession("capacity", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "capacity", sitos::fence_test::kAttachGeneration));
  const auto cleaned_token = sitos::fence_test::Token(std::byte{0x72});
  transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "capacity", sitos::fence_test::kAttachGeneration, sitos::BufferClass::Ephemeral,
      rejected_capacity_publisher, sitos::AckDurability::Applied, 0, cleaned_token));
  const auto cleaned =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, cleaned_token);
  ASSERT_TRUE(cleaned.has_value());
  ExpectFenceResult(*cleaned, sitos::Status::Ok, 0, sitos::kAckNoFailedSequence);
}

}  // namespace
