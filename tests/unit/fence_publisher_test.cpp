// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
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

class FinalGenerationGateTransport final : public sitos::Transport {
 public:
  bool SupportsFenceProfile() const noexcept override { return true; }

  std::uint64_t FenceGeneration() const noexcept override {
    std::function<void()> generation_observer;
    std::uint64_t generation = 0;
    {
      std::unique_lock lock(mutex_);
      if (marker_submitted_ && ++post_marker_generation_reads_ == gate_generation_read_ &&
          gate_final_generation_sample_) {
        generation_gate_entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return release_generation_gate_; });
      }
      generation = generation_;
      if (marker_submitted_ && generation_observer_ && !generation_observer_fired_) {
        generation_observer_fired_ = true;
        generation_observer = generation_observer_;
      }
    }
    if (generation_observer) generation_observer();
    return generation;
  }

  std::shared_ptr<sitos::fence_internal::FenceDispatchCoordinator> FenceDispatcher() noexcept
      override {
    return dispatcher_;
  }

  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding encoding,
                          sitos::PutOptions options) override {
    std::function<void(const sitos::Encoding&, const sitos::PutOptions&)> observer;
    {
      std::scoped_lock lock(mutex_);
      marker_submitted_at_ = std::chrono::steady_clock::now();
      marker_submitted_ = true;
      post_marker_generation_reads_ = 0;
      observer = put_observer_;
    }
    if (observer) observer(encoding, options);
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view, const QueryResultSink&,
                          std::chrono::milliseconds) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)>) override {
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }
  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

  void SetPutObserver(
      std::function<void(const sitos::Encoding&, const sitos::PutOptions&)> observer) {
    std::scoped_lock lock(mutex_);
    put_observer_ = std::move(observer);
  }

  // Blocks the chosen post-marker generation read. Counting restarts here so a
  // caller can arm the gate after earlier reads have already been observed.
  void GateFinalGenerationSample(std::size_t post_marker_read = 2) {
    std::scoped_lock lock(mutex_);
    gate_final_generation_sample_ = true;
    gate_generation_read_ = post_marker_read;
    post_marker_generation_reads_ = 0;
  }

  void SetGenerationObserver(std::function<void()> observer) {
    std::scoped_lock lock(mutex_);
    generation_observer_ = std::move(observer);
    generation_observer_fired_ = false;
  }

  bool WaitForGenerationGate(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return generation_gate_entered_; });
  }

  void ReleaseGenerationGate() {
    {
      std::scoped_lock lock(mutex_);
      release_generation_gate_ = true;
    }
    condition_.notify_all();
  }

  std::chrono::steady_clock::time_point MarkerSubmittedAt() const {
    std::scoped_lock lock(mutex_);
    return marker_submitted_at_;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable std::size_t post_marker_generation_reads_ = 0;
  mutable bool generation_gate_entered_ = false;
  mutable bool release_generation_gate_ = false;
  mutable bool generation_observer_fired_ = false;
  bool gate_final_generation_sample_ = false;
  std::size_t gate_generation_read_ = 2;
  bool marker_submitted_ = false;
  std::uint64_t generation_ = 1;
  std::chrono::steady_clock::time_point marker_submitted_at_{};
  std::function<void(const sitos::Encoding&, const sitos::PutOptions&)> put_observer_;
  std::function<void()> generation_observer_;
  std::shared_ptr<sitos::fence_internal::FenceDispatchCoordinator> dispatcher_ =
      std::make_shared<sitos::fence_internal::FenceDispatchCoordinator>();
};

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

  // Once terminal publication has sampled the original generation, a later
  // replacement before marker Put returns cannot discard that immutable result.
  auto completed_then_replaced_transport = sitos::fence_test::MakeTransport();
  auto completed_then_replaced_publisher =
      sitos::fence_test_access::FenceTestAccess::CreatePublisher(
          *completed_then_replaced_transport, sitos::fence_test::kPublisherB,
          sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
              sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  completed_then_replaced_transport->SetPutObserver(
      [&completed_then_replaced_transport, &completed_then_replaced_publisher](const auto& record) {
        if (record.encoding.id == sitos::Encoding::kSitosV1Fence &&
            record.options.ack_token.has_value()) {
          sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
              completed_then_replaced_publisher, *record.options.ack_token);
          completed_then_replaced_transport->ReplaceGeneration();
        }
      });
  auto completed_then_replaced =
      completed_then_replaced_publisher.BeginFence(sitos::fence_test::kDeadline);
  ASSERT_TRUE(completed_then_replaced.IsOk())
      << "post-Put generation detection discarded a terminal result";
  const auto retained_completion =
      completed_then_replaced_publisher.Wait(completed_then_replaced.Value());
  ASSERT_TRUE(retained_completion.IsOk());
  EXPECT_EQ(retained_completion.Value().status, sitos::Status::Ok);
  EXPECT_EQ(completed_then_replaced_publisher.SubmitPut().StatusCode(),
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

  // ADR-0029 step 6: the total deadline starts immediately before the sole marker
  // Put, after waiter publication. Time spent waiting for admission and the lane
  // therefore does not consume the caller's Fence budget, and a completion shortly
  // after submission still succeeds even when the lane delay exceeded the deadline.
  auto lane_deadline_transport = sitos::fence_test::MakeTransport();
  auto lane_deadline_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *lane_deadline_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  std::promise<void> data_put_entered;
  auto data_put_entered_future = data_put_entered.get_future();
  std::promise<void> release_data_put;
  auto release_data_put_future = release_data_put.get_future().share();
  lane_deadline_transport->SetPutObserver(
      [&lane_deadline_publisher, &data_put_entered, &release_data_put_future](const auto& record) {
        if (record.encoding.id == sitos::Encoding::kSitosV1Fence) {
          if (record.options.ack_token.has_value()) {
            sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
                lane_deadline_publisher, *record.options.ack_token);
          }
          return;
        }
        data_put_entered.set_value();
        release_data_put_future.wait();
      });
  auto blocking_data_put =
      std::async(std::launch::async, [&] { return lane_deadline_publisher.SubmitPut(); });
  if (data_put_entered_future.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    release_data_put.set_value();
    FAIL() << "data Put did not acquire the publisher lane";
  }
  constexpr auto lane_total_deadline = std::chrono::milliseconds{50};
  lane_deadline_publisher.GateNextOperation();
  auto lane_deadline_begin = std::async(
      std::launch::async, [&] { return lane_deadline_publisher.BeginFence(lane_total_deadline); });
  lane_deadline_publisher.WaitForGatedOperation();
  // Hold the Fence behind the lane for longer than its entire deadline.
  WaitUntilDeadline(std::chrono::steady_clock::now() + 2 * lane_total_deadline);
  lane_deadline_publisher.ReleaseGatedOperation();
  release_data_put.set_value();
  ASSERT_TRUE(blocking_data_put.get().IsOk());
  auto lane_deadline_handle = lane_deadline_begin.get();
  ASSERT_TRUE(lane_deadline_handle.IsOk());
  EXPECT_TRUE(lane_deadline_publisher.Wait(lane_deadline_handle.Value()).IsOk())
      << "lane admission delay must not consume the total deadline";

  // Completion must not hold the waiter mutex while sampling Transport generation.
  // A synchronizing Transport can otherwise deadlock when generation sampling
  // re-enters completion through another callback.
  FinalGenerationGateTransport lock_order_transport;
  auto lock_order_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      lock_order_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  auto lock_order_handle = lock_order_publisher.BeginFence(std::chrono::seconds{2});
  ASSERT_TRUE(lock_order_handle.IsOk());
  std::future<void> concurrent_close;
  bool close_completed_before_generation_return = false;
  lock_order_transport.SetGenerationObserver([&] {
    concurrent_close = std::async(std::launch::async, [&] { lock_order_publisher.Close(); });
    close_completed_before_generation_return =
        concurrent_close.wait_for(std::chrono::milliseconds{250}) == std::future_status::ready;
  });
  auto sampled_completion = std::async(std::launch::async, [&] {
    sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
        lock_order_publisher, lock_order_handle.Value().token);
  });
  ASSERT_EQ(sampled_completion.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  sampled_completion.get();
  ASSERT_TRUE(concurrent_close.valid());
  ASSERT_EQ(concurrent_close.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  concurrent_close.get();
  EXPECT_TRUE(close_completed_before_generation_return)
      << "Transport generation sampling must not run under either waiter lock";
  const auto cancelled_completion = lock_order_publisher.Wait(lock_order_handle.Value());
  ASSERT_TRUE(cancelled_completion.IsOk());
  EXPECT_EQ(cancelled_completion.Value().status, sitos::Status::Disconnected)
      << "Close must beat a completion that has not terminally published";

  // Terminal publication does not linearize until the final generation sample
  // finishes. Crossing the deadline inside that sample must therefore time out.
  FinalGenerationGateTransport final_sample_transport;
  auto final_sample_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      final_sample_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  final_sample_transport.SetPutObserver(
      [&final_sample_publisher](const auto& encoding, const auto& options) {
        if (encoding.id == sitos::Encoding::kSitosV1Fence && options.ack_token.has_value()) {
          sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(final_sample_publisher,
                                                                            *options.ack_token);
        }
      });
  final_sample_transport.GateFinalGenerationSample();
  constexpr auto final_sample_deadline = std::chrono::milliseconds{50};
  auto final_sample_begin = std::async(
      std::launch::async, [&] { return final_sample_publisher.BeginFence(final_sample_deadline); });
  if (!final_sample_transport.WaitForGenerationGate(std::chrono::seconds{2})) {
    final_sample_transport.ReleaseGenerationGate();
    FAIL() << "final generation sample did not reach the deterministic gate";
  }
  WaitUntilDeadline(final_sample_transport.MarkerSubmittedAt() + final_sample_deadline);
  final_sample_transport.ReleaseGenerationGate();
  auto final_sample_handle = final_sample_begin.get();
  ASSERT_TRUE(final_sample_handle.IsOk());
  const auto final_sample_result = final_sample_publisher.Wait(final_sample_handle.Value());
  EXPECT_EQ(final_sample_result.StatusCode(), sitos::Status::Timeout)
      << "a completion cannot use a timestamp from before final generation validation";

  // Generation sampling must not hold the waiter mutex, and the completion
  // timestamp must be captured only after terminal publication wins that mutex.
  FinalGenerationGateTransport publication_transport;
  auto publication_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      publication_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  constexpr auto publication_deadline = std::chrono::milliseconds{50};
  auto publication_handle = publication_publisher.BeginFence(publication_deadline);
  ASSERT_TRUE(publication_handle.IsOk());
  std::promise<void> waiter_mutex_locked;
  auto waiter_mutex_locked_future = waiter_mutex_locked.get_future();
  std::promise<void> release_waiter_mutex;
  auto release_waiter_mutex_future = release_waiter_mutex.get_future().share();
  std::promise<void> publication_blocked;
  auto publication_blocked_future = publication_blocked.get_future();
  std::future<void> waiter_mutex_blocker;
  publication_transport.SetGenerationObserver([&] {
    waiter_mutex_blocker = std::async(std::launch::async, [&] {
      std::scoped_lock lock(publication_handle.Value().waiter->mutex);
      waiter_mutex_locked.set_value();
      release_waiter_mutex_future.wait();
    });
    ASSERT_EQ(waiter_mutex_locked_future.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    publication_blocked.set_value();
  });
  auto delayed_publication = std::async(std::launch::async, [&] {
    sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
        publication_publisher, publication_handle.Value().token);
  });
  if (publication_blocked_future.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    release_waiter_mutex.set_value();
    FAIL() << "waiter mutex blocker did not run during generation sampling";
  }
  WaitUntilDeadline(publication_handle.Value().deadline);
  release_waiter_mutex.set_value();
  ASSERT_TRUE(waiter_mutex_blocker.valid());
  waiter_mutex_blocker.get();
  delayed_publication.get();
  const auto publication_result = publication_publisher.Wait(publication_handle.Value());
  EXPECT_EQ(publication_result.StatusCode(), sitos::Status::Timeout)
      << "terminal publication after the deadline cannot retain a pre-lock timestamp";

  // A completion that has only reserved the waiter slot carries a provisional result
  // that has not yet passed the final generation validation. A wait whose deadline
  // expires inside that reservation window must report Timeout, never that provisional
  // result: publishing it would make a success visible before the validation that
  // DEC-99-GENERATION-SAMPLING-001 requires to be able to downgrade it.
  FinalGenerationGateTransport reservation_transport;
  auto reservation_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      reservation_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  constexpr auto reservation_deadline = std::chrono::milliseconds{500};
  auto reservation_handle = reservation_publisher.BeginFence(reservation_deadline);
  ASSERT_TRUE(reservation_handle.IsOk());
  std::promise<void> reservation_wait_sampled;
  auto reservation_wait_sampled_future = reservation_wait_sampled.get_future();
  reservation_transport.SetGenerationObserver([&] { reservation_wait_sampled.set_value(); });
  auto reservation_wait = std::async(
      std::launch::async, [&] { return reservation_publisher.Wait(reservation_handle.Value()); });
  ASSERT_EQ(reservation_wait_sampled_future.wait_for(std::chrono::seconds{2}),
            std::future_status::ready)
      << "the wait must sample the Transport generation before the completion runs";
  // Counting restarts at the arming point: read 1 is the reservation sample and read 2
  // is the final validation that precedes terminal publication.
  reservation_transport.GateFinalGenerationSample(2);
  auto reservation_completion = std::async(std::launch::async, [&] {
    sitos::fence_test_access::FenceTestAccess::CompletePublisherFence(
        reservation_publisher, reservation_handle.Value().token);
  });
  if (!reservation_transport.WaitForGenerationGate(std::chrono::seconds{2})) {
    reservation_transport.ReleaseGenerationGate();
    reservation_completion.get();
    static_cast<void>(reservation_wait.get());
    FAIL() << "the final generation validation did not reach the deterministic gate";
  }
  ASSERT_LT(std::chrono::steady_clock::now(), reservation_handle.Value().deadline)
      << "the reservation must still be in flight when the wait deadline is crossed";
  WaitUntilDeadline(reservation_handle.Value().deadline);
  // The wait must resolve on its own deadline while the reservation is still gated, so
  // the observed result is the one the waiter chose with the validation outstanding.
  ASSERT_EQ(reservation_wait.wait_for(std::chrono::seconds{2}), std::future_status::ready)
      << "the wait did not time out while the final generation validation was gated";
  const auto reservation_result = reservation_wait.get();
  reservation_transport.ReleaseGenerationGate();
  reservation_completion.get();
  EXPECT_EQ(reservation_result.StatusCode(), sitos::Status::Timeout)
      << "a wait that expires inside the reservation window must not publish the "
         "provisional result";

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

  // Timeout diagnostics are frozen with the Fence prefix. A later excluded write
  // must not replace the covered diagnostic before Wait observes the timeout.
  auto diagnostic_transport = sitos::fence_test::MakeTransport();
  auto diagnostic_publisher = sitos::fence_test_access::FenceTestAccess::CreatePublisher(
      *diagnostic_transport, sitos::fence_test::kPublisherA,
      sitos::fence_test_access::FenceTestAccess::CacheReceiverBinding(
          sitos::fence_test::kSid, sitos::fence_test::kAttachGeneration));
  diagnostic_transport->SetDataSubmissionResult(
      sitos::Result<void>::Err(std::make_error_code(std::errc::io_error)));
  ASSERT_FALSE(diagnostic_publisher.SubmitPut().IsOk());
  diagnostic_transport->SetDataSubmissionResult(sitos::Result<void>::Ok());
  auto diagnostic_handle = diagnostic_publisher.BeginFence(std::chrono::milliseconds{20});
  ASSERT_TRUE(diagnostic_handle.IsOk());
  diagnostic_transport->SetDataSubmissionResult(
      sitos::Result<void>::Err(std::make_error_code(std::errc::permission_denied)));
  std::promise<void> later_write_entered;
  auto later_write_entered_future = later_write_entered.get_future();
  std::promise<void> release_later_write;
  auto release_later_write_future = release_later_write.get_future().share();
  diagnostic_transport->SetPutObserver([&](const auto&) {
    later_write_entered.set_value();
    release_later_write_future.wait();
  });
  auto later_write =
      std::async(std::launch::async, [&] { return diagnostic_publisher.SubmitPut(); });
  if (later_write_entered_future.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    release_later_write.set_value();
    FAIL() << "excluded later write did not reach its Transport Put";
  }
  auto diagnostic_wait = std::async(
      std::launch::async, [&] { return diagnostic_publisher.Wait(diagnostic_handle.Value()); });
  if (diagnostic_wait.wait_for(std::chrono::milliseconds{250}) != std::future_status::ready) {
    release_later_write.set_value();
    FAIL() << "Wait blocked behind an excluded later write";
  }
  const auto diagnostic_timeout = diagnostic_wait.get();
  ASSERT_EQ(diagnostic_timeout.StatusCode(), sitos::Status::Timeout);
  EXPECT_EQ(diagnostic_timeout.Error(), std::make_error_code(std::errc::io_error))
      << "an excluded later write cannot replace the covered timeout diagnostic";
  release_later_write.set_value();
  ASSERT_FALSE(later_write.get().IsOk());

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
