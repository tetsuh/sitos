// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "fence_test_support.hpp"

namespace {

void WaitUntilDeadline(std::chrono::steady_clock::time_point deadline) {
  std::mutex mutex;
  std::condition_variable condition;
  std::unique_lock lock(mutex);
  static_cast<void>(condition.wait_until(lock, deadline, [] { return false; }));
}

TEST(FencePublisherTest, LinearizesDataAndMarkerAndBoundsAdmission) {
  auto transport = sitos::fence_test::MakeTransport();
  auto publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  transport->SetPutObserver([&publisher](const auto& record) {
    if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
        record.options.ack_token.has_value()) {
      sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(publisher,
                                                                        *record.options.ack_token);
    }
  });

  transport->SetDataSubmissionResult(
      sitos::Result<void>::Err(std::make_error_code(std::errc::io_error)));
  EXPECT_FALSE(publisher.SubmitPut().IsOk());
  EXPECT_EQ(publisher.last_sequence(), 1U);
  EXPECT_TRUE(publisher.may_have_submitted());
  EXPECT_EQ(transport->DataSequences(), (std::vector<std::uint64_t>{1}));

  transport->SetDataSubmissionResult(sitos::Result<void>::Ok());
  ASSERT_TRUE(publisher.SubmitBatch().IsOk());
  EXPECT_EQ(publisher.last_sequence(), 2U);

  // Keep the marker's callback pending. The second Fence is rejected only while
  // this first waiter remains registered; later data is legal but excluded.
  transport->GateMarkerCompletion();
  auto first = publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(first.IsOk());
  EXPECT_EQ(first.Value().through_sequence, 2U);
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::PublisherWaiterPublished(
      publisher, first.Value().token));
  EXPECT_EQ(transport->MarkerCount(), 1U);

  const auto second_while_pending = publisher.BeginFence(sitos::fence_test::kDeadline);
  EXPECT_EQ(second_while_pending.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(second_while_pending.Error(), std::make_error_code(std::errc::operation_in_progress));
  EXPECT_EQ(transport->MarkerCount(), 1U);
  EXPECT_EQ(transport->TokenCount(), 1U);

  ASSERT_TRUE(publisher.SubmitPut().IsOk());
  EXPECT_EQ(publisher.last_sequence(), 3U);
  EXPECT_EQ(transport->DataSequences(), (std::vector<std::uint64_t>{1, 2, 3}));
  transport->ReleaseMarkerCompletion();
  const auto first_result = publisher.Wait(first.Value());
  ASSERT_TRUE(first_result.IsOk());
  EXPECT_EQ(first_result.Value().through_sequence, 2U);

  // A completed Fence no longer occupies the one-waiter slot.
  auto sequential = publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(sequential.IsOk());
  EXPECT_EQ(sequential.Value().through_sequence, 3U);
  EXPECT_EQ(transport->MarkerCount(), 2U);
  EXPECT_TRUE(publisher.Wait(sequential.Value()).IsOk());

  // Concurrent sender methods share one Publisher linearization lane. The
  // marker can linearize anywhere, but data sequence allocation stays unique
  // and a later marker covers the complete prefix.
  auto concurrent_transport = sitos::fence_test::MakeTransport();
  auto concurrent_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *concurrent_transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  concurrent_transport->SetPutObserver([&concurrent_publisher](const auto& record) {
    if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
        record.options.ack_token.has_value()) {
      sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(concurrent_publisher,
                                                                        *record.options.ack_token);
    }
  });
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  auto concurrent_put = std::async(std::launch::async, [&] {
    start.wait();
    return concurrent_publisher.SubmitPut();
  });
  auto concurrent_batch = std::async(std::launch::async, [&] {
    start.wait();
    return concurrent_publisher.SubmitBatch();
  });
  auto concurrent_push = std::async(std::launch::async, [&] {
    start.wait();
    return concurrent_publisher.SubmitPush();
  });
  auto concurrent_fence = std::async(std::launch::async, [&] {
    start.wait();
    return concurrent_publisher.BeginFence(sitos::fence_test::kDeadline);
  });
  start_promise.set_value();
  EXPECT_TRUE(concurrent_put.get().IsOk());
  EXPECT_TRUE(concurrent_batch.get().IsOk());
  EXPECT_TRUE(concurrent_push.get().IsOk());
  auto concurrent_handle = concurrent_fence.get();
  ASSERT_TRUE(concurrent_handle.IsOk());
  EXPECT_LE(concurrent_handle.Value().through_sequence, 3U);
  EXPECT_TRUE(concurrent_publisher.Wait(concurrent_handle.Value()).IsOk());
  EXPECT_EQ(concurrent_transport->DataSequences(), (std::vector<std::uint64_t>{1, 2, 3}));
  auto complete_prefix = concurrent_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(complete_prefix.IsOk());
  EXPECT_EQ(complete_prefix.Value().through_sequence, 3U);
  EXPECT_TRUE(concurrent_publisher.Wait(complete_prefix.Value()).IsOk());

  // Close completes and joins an active wait before returning.
  auto close_transport = sitos::fence_test::MakeTransport();
  auto close_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *close_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  auto close_handle = close_publisher.BeginFence(std::chrono::seconds(1));
  ASSERT_TRUE(close_handle.IsOk());
  std::atomic<bool> wait_started{false};
  auto active_wait = std::async(std::launch::async, [&] {
    wait_started = true;
    return close_publisher.Wait(close_handle.Value());
  });
  EXPECT_TRUE(sitos::fence_test::WaitUntil(
      [&] { return wait_started.load() && close_publisher.ActiveWaits() == 1U; },
      std::chrono::seconds(1)))
      << "the active wait must be admitted within the test deadline";
  close_publisher.Close();
  EXPECT_EQ(close_publisher.ActiveWaits(), 0U);
  EXPECT_EQ(active_wait.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
  const auto closed_wait = active_wait.get();
  ASSERT_TRUE(closed_wait.IsOk());
  EXPECT_EQ(closed_wait.Value().status, sitos::Status::Disconnected);

  // One generation-mismatch observer quiesces multiple admitted waits without
  // making every waiter block on the same active-count predicate.
  auto multi_transport = sitos::fence_test::MakeTransport();
  auto multi_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *multi_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  auto multi_handle = multi_publisher.BeginFence(std::chrono::seconds(1));
  ASSERT_TRUE(multi_handle.IsOk());
  auto first_wait =
      std::async(std::launch::async, [&] { return multi_publisher.Wait(multi_handle.Value()); });
  auto second_wait =
      std::async(std::launch::async, [&] { return multi_publisher.Wait(multi_handle.Value()); });
  EXPECT_TRUE(sitos::fence_test::WaitUntil([&] { return multi_publisher.ActiveWaits() == 2U; },
                                           std::chrono::seconds(1)))
      << "both waits must be admitted within the test deadline";
  multi_transport->ReplaceGeneration();
  auto detect_replacement =
      std::async(std::launch::async, [&] { return multi_publisher.SubmitPut(); });
  EXPECT_EQ(detect_replacement.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
  EXPECT_EQ(detect_replacement.get().StatusCode(), sitos::Status::Disconnected);
  const auto first_disconnected = first_wait.get();
  const auto second_disconnected = second_wait.get();
  ASSERT_TRUE(first_disconnected.IsOk());
  ASSERT_TRUE(second_disconnected.IsOk());
  EXPECT_EQ(first_disconnected.Value().status, sitos::Status::Disconnected);
  EXPECT_EQ(second_disconnected.Value().status, sitos::Status::Disconnected);
  EXPECT_EQ(multi_publisher.ActiveWaits(), 0U);

  // SubmitData and BeginFence participate in the same lifecycle admission as
  // Wait. Close cannot return while either operation was admitted before the
  // gate closed, even when it has not acquired the serialization lane yet.
  auto admitted_data_transport = sitos::fence_test::MakeTransport();
  auto admitted_data_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *admitted_data_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  admitted_data_publisher.GateNextOperation();
  auto admitted_data =
      std::async(std::launch::async, [&] { return admitted_data_publisher.SubmitPut(); });
  admitted_data_publisher.WaitForGatedOperation();
  auto close_admitted_data =
      std::async(std::launch::async, [&] { admitted_data_publisher.Close(); });
  EXPECT_EQ(close_admitted_data.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  admitted_data_publisher.ReleaseGatedOperation();
  EXPECT_EQ(admitted_data.get().StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(close_admitted_data.wait_for(std::chrono::milliseconds(100)),
            std::future_status::ready);
  EXPECT_EQ(admitted_data_transport->DataSubmissionCount(), 0U);
  EXPECT_EQ(admitted_data_publisher.ActiveWaits(), 0U);

  auto admitted_fence_transport = sitos::fence_test::MakeTransport();
  auto admitted_fence_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *admitted_fence_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  admitted_fence_publisher.GateNextOperation();
  auto admitted_fence = std::async(std::launch::async, [&] {
    return admitted_fence_publisher.BeginFence(sitos::fence_test::kDeadline);
  });
  admitted_fence_publisher.WaitForGatedOperation();
  auto close_admitted_fence =
      std::async(std::launch::async, [&] { admitted_fence_publisher.Close(); });
  EXPECT_EQ(close_admitted_fence.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  admitted_fence_publisher.ReleaseGatedOperation();
  EXPECT_EQ(admitted_fence.get().StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(close_admitted_fence.wait_for(std::chrono::milliseconds(100)),
            std::future_status::ready);
  EXPECT_EQ(admitted_fence_transport->MarkerSubmissionCount(), 0U);
  EXPECT_EQ(admitted_fence_publisher.ActiveWaits(), 0U);

  auto max_transport = sitos::fence_test::MakeTransport();
  auto max_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *max_transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  max_transport->SetPutObserver([&max_publisher](const auto& record) {
    if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
        record.options.ack_token.has_value()) {
      sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(max_publisher,
                                                                        *record.options.ack_token);
    }
  });
  sitos::fence_test_access::FenceTestAccess::SetLastSequenceForTesting(max_publisher,
                                                                       UINT64_MAX - 1);
  ASSERT_TRUE(max_publisher.SubmitPut().IsOk());
  EXPECT_EQ(max_publisher.last_sequence(), UINT64_MAX);
  EXPECT_TRUE(max_publisher.is_exhausted());
  EXPECT_EQ(max_transport->DataSequences(), (std::vector<std::uint64_t>{UINT64_MAX}));
  const auto rejected_after_max = max_publisher.SubmitPut();
  EXPECT_EQ(rejected_after_max.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(max_transport->DataSubmissionCount(), 1U);

  auto max_fence = max_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(max_fence.IsOk());
  EXPECT_EQ(max_fence.Value().through_sequence, UINT64_MAX);
  EXPECT_TRUE(max_publisher.Wait(max_fence.Value()).IsOk());

  // A generation replacement that overlaps data submission makes the possibly
  // submitted call and every later operation permanently disconnected.
  auto data_replacement_transport = sitos::fence_test::MakeTransport();
  auto data_replacement_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *data_replacement_transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  data_replacement_transport->SetPutObserver([&data_replacement_transport](const auto& record) {
    if (record.encoding.id != sitos::Encoding::kSitosV1Fence) {
      data_replacement_transport->ReplaceGeneration();
    }
  });
  EXPECT_EQ(data_replacement_publisher.SubmitPut().StatusCode(), sitos::Status::Disconnected);
  EXPECT_TRUE(data_replacement_publisher.may_have_submitted());
  EXPECT_EQ(data_replacement_transport->DataSubmissionCount(), 1U);
  EXPECT_EQ(data_replacement_publisher.BeginFence(sitos::fence_test::kDeadline).StatusCode(),
            sitos::Status::Disconnected);
  EXPECT_EQ(data_replacement_transport->MarkerSubmissionCount(), 0U);

  // A generation replacement that overlaps marker submission must not let a
  // synchronous completion from the replacement generation prove the old lane.
  auto synchronous_replacement_transport = sitos::fence_test::MakeTransport();
  auto synchronous_replacement_publisher =
      sitos::fence_test_access::FenceTestAccess::CreatePublisher(
          *synchronous_replacement_transport, sitos::fence_test::kPublisherA,
          sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
              sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  ASSERT_TRUE(synchronous_replacement_publisher.SubmitPut().IsOk());
  synchronous_replacement_transport->SetPutObserver(
      [&synchronous_replacement_transport, &synchronous_replacement_publisher](const auto& record) {
        if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
            record.options.ack_token.has_value()) {
          synchronous_replacement_transport->ReplaceGeneration();
          sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
              synchronous_replacement_publisher, *record.options.ack_token);
        }
      });
  EXPECT_EQ(synchronous_replacement_publisher.BeginFence(sitos::fence_test::kDeadline).StatusCode(),
            sitos::Status::Disconnected);
  EXPECT_EQ(synchronous_replacement_publisher.SubmitPut().StatusCode(),
            sitos::Status::Disconnected);

  // The same boundary applies when completion is delayed until after Put
  // returns. In contrast, an Ok completion observed before replacement remains
  // immutable (covered near the end of this test).
  auto asynchronous_replacement_transport = sitos::fence_test::MakeTransport();
  auto asynchronous_replacement_publisher =
      sitos::fence_test_access::FenceTestAccess::CreatePublisher(
          *asynchronous_replacement_transport, sitos::fence_test::kPublisherB,
          sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
              sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  asynchronous_replacement_transport->SetPutObserver(
      [&asynchronous_replacement_publisher](const auto& record) {
        if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
            record.options.ack_token.has_value()) {
          sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
              asynchronous_replacement_publisher, *record.options.ack_token);
        }
      });
  asynchronous_replacement_transport->GateMarkerCompletion();
  auto asynchronous_replacement =
      asynchronous_replacement_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(asynchronous_replacement.IsOk());
  asynchronous_replacement_transport->ReplaceGeneration();
  asynchronous_replacement_transport->ReleaseMarkerCompletion();
  const auto asynchronous_replacement_result =
      asynchronous_replacement_publisher.Wait(asynchronous_replacement.Value());
  ASSERT_TRUE(asynchronous_replacement_result.IsOk());
  EXPECT_EQ(asynchronous_replacement_result.Value().status, sitos::Status::Disconnected);
  EXPECT_EQ(asynchronous_replacement_publisher.SubmitPut().StatusCode(),
            sitos::Status::Disconnected);

  // Completion publication samples the generation again before exposing the
  // terminal result. This deterministic transport replacement occurs after the
  // first sample and must still downgrade the hidden provisional Ok result.
  auto completion_race_transport = sitos::fence_test::MakeTransport();
  auto completion_race_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *completion_race_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  completion_race_transport->SetPutObserver([&completion_race_publisher](const auto& record) {
    if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
        record.options.ack_token.has_value()) {
      sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(completion_race_publisher,
                                                                        *record.options.ack_token);
    }
  });
  completion_race_transport->GateMarkerCompletion();
  auto completion_race = completion_race_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(completion_race.IsOk());
  completion_race_transport->ReplaceGenerationAfterReads(1);
  completion_race_transport->ReleaseMarkerCompletion();
  const auto completion_race_result = completion_race_publisher.Wait(completion_race.Value());
  ASSERT_TRUE(completion_race_result.IsOk());
  EXPECT_EQ(completion_race_result.Value().status, sitos::Status::Disconnected);

  // A buffer acknowledgement observed at or after the deadline must not be
  // restored as a successful waiter result after PollAcknowledgement returns.
  auto late_buffer_transport = sitos::fence_test::MakeTransport();
  auto late_buffer_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *late_buffer_transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::BufferReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kSessionGeneration,
          sitos::BufferClass::Durable, sitos::AckDurability::Applied));
  std::promise<void> late_query_entered;
  auto late_query_entered_future = late_query_entered.get_future();
  std::promise<void> release_late_query;
  auto release_late_query_future = release_late_query.get_future().share();
  late_buffer_transport->DeclareQueryable(
      "sitos/meta/ack/**", [&release_late_query_future, &late_query_entered](auto& query) {
        late_query_entered.set_value();
        release_late_query_future.wait();
        const auto acknowledgement =
            sitos::fence_test::FenceResult(sitos::Status::Ok, sitos::AckDurability::Applied, 0);
        const auto encoded = sitos::EncodeAckResult(acknowledgement);
        EXPECT_TRUE(encoded.IsOk());
        if (encoded.IsOk()) {
          EXPECT_TRUE(query
                          .Reply(query.keyexpr, encoded.Value(),
                                 sitos::Encoding{std::string(sitos::Encoding::kSitosV1Ack)})
                          .IsOk());
        }
      });
  auto late_buffer_handle = late_buffer_publisher.BeginFence(std::chrono::milliseconds{250});
  ASSERT_TRUE(late_buffer_handle.IsOk());
  auto late_buffer_wait = std::async(
      std::launch::async, [&] { return late_buffer_publisher.Wait(late_buffer_handle.Value()); });
  if (late_query_entered_future.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    release_late_query.set_value();
    FAIL() << "buffer acknowledgement query did not start";
  }
  WaitUntilDeadline(late_buffer_handle.Value().deadline);
  release_late_query.set_value();
  ASSERT_EQ(late_buffer_wait.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  const auto late_buffer_result = late_buffer_wait.get();
  ASSERT_FALSE(late_buffer_result.IsOk())
      << "a buffer completion at the deadline must not be restored as success";
  EXPECT_EQ(late_buffer_result.StatusCode(), sitos::Status::Timeout);

  // A completion published after the deadline cannot survive the no-admission path.
  auto late_closed_transport = sitos::fence_test::MakeTransport();
  auto late_closed_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *late_closed_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  auto late_closed_handle = late_closed_publisher.BeginFence(std::chrono::milliseconds{100});
  ASSERT_TRUE(late_closed_handle.IsOk());
  WaitUntilDeadline(late_closed_handle.Value().deadline);
  sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
      late_closed_publisher, late_closed_handle.Value().token);
  late_closed_publisher.Close();
  const auto late_closed_result = late_closed_publisher.Wait(late_closed_handle.Value());
  ASSERT_FALSE(late_closed_result.IsOk());
  EXPECT_EQ(late_closed_result.StatusCode(), sitos::Status::Timeout);

  auto buffer_transport = sitos::fence_test::MakeTransport();
  auto buffer_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *buffer_transport, sitos::fence_test::kPublisherB,
      sitos::fence_test_access::FenceTestAccess::BufferReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kSessionGeneration,
          sitos::BufferClass::Durable, sitos::AckDurability::Applied));
  ASSERT_TRUE(buffer_publisher.SubmitPush().IsOk());
  EXPECT_EQ(buffer_transport->DataSequences(), (std::vector<std::uint64_t>{1}));
  auto disconnected_buffer_fence = buffer_publisher.BeginFence(std::chrono::seconds(1));
  ASSERT_TRUE(disconnected_buffer_fence.IsOk());
  buffer_transport->ReplaceGeneration();
  const auto disconnected_buffer_result = buffer_publisher.Wait(disconnected_buffer_fence.Value());
  ASSERT_TRUE(disconnected_buffer_result.IsOk());
  EXPECT_EQ(disconnected_buffer_result.Value().status, sitos::Status::Disconnected);

  auto timeout_transport = sitos::fence_test::MakeTransport();
  auto timeout_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *timeout_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  timeout_transport->SetDataSubmissionResult(
      sitos::Result<void>::Err(std::make_error_code(std::errc::io_error)));
  auto timed_out = timeout_publisher.BeginFence(std::chrono::milliseconds(5));
  ASSERT_TRUE(timed_out.IsOk());
  EXPECT_EQ(timeout_transport->MarkerSubmissionCount(), 1U);
  const auto timeout_result = timeout_publisher.Wait(timed_out.Value());
  EXPECT_EQ(timeout_result.StatusCode(), sitos::Status::Timeout);
  EXPECT_EQ(timeout_result.Error(), std::make_error_code(std::errc::io_error));
  EXPECT_TRUE(timeout_publisher.may_have_submitted());
  EXPECT_EQ(timeout_transport->MarkerSubmissionCount(), 1U)
      << "a possibly submitted marker is never resubmitted";
  sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(timeout_publisher,
                                                                    timed_out.Value().token);
  const auto immutable_timeout = timeout_publisher.Wait(timed_out.Value());
  EXPECT_EQ(immutable_timeout.StatusCode(), sitos::Status::Timeout)
      << "late completion cannot revive a timed-out waiter";
  EXPECT_EQ(timeout_transport->MarkerSubmissionCount(), 1U);

  const auto invalid_deadline = publisher.BeginFence(std::chrono::milliseconds::zero());
  EXPECT_EQ(invalid_deadline.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport->MarkerCount(), 2U);

  transport->ReplaceGeneration();
  EXPECT_EQ(publisher.SubmitPut().StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(publisher.BeginFence(sitos::fence_test::kDeadline).StatusCode(),
            sitos::Status::Disconnected);
  EXPECT_EQ(publisher.BeginFence(std::chrono::milliseconds::zero()).StatusCode(),
            sitos::Status::Disconnected);
  const auto immutable_after_disconnect = publisher.Wait(sequential.Value());
  ASSERT_TRUE(immutable_after_disconnect.IsOk());
  EXPECT_EQ(immutable_after_disconnect.Value().status, sitos::Status::Ok);
  EXPECT_EQ(transport->DataSubmissionCount(), 3U);
}

}  // namespace
