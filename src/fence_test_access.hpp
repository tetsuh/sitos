// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Source-private deterministic access for Issue #158 tests.

#ifndef SITOS_FENCE_TEST_ACCESS_HPP
#define SITOS_FENCE_TEST_ACCESS_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fence_internal.hpp"
#include "sitos/batch.hpp"
#include "sitos/key.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/param_value.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace sitos::fence_test_access {
using namespace fence_internal;

class FencePublisherHarness {
 public:
  explicit FencePublisherHarness(std::unique_ptr<fence_internal::FencePublisher> publisher)
      : publisher_(std::move(publisher)) {}

  Result<void> SubmitPut() { return publisher_->SubmitForTesting("put"); }
  Result<void> SubmitBatch() { return publisher_->SubmitForTesting("batch"); }
  Result<void> SubmitPush() { return publisher_->SubmitForTesting("push"); }
  Result<fence_internal::FenceHandle> BeginFence(std::chrono::milliseconds deadline) {
    return publisher_->BeginFence(deadline);
  }
  Result<AckResultV1> Wait(const fence_internal::FenceHandle& handle) {
    return publisher_->Wait(handle);
  }
  void Close() { publisher_->Close(); }
  std::size_t ActiveWaits() const { return publisher_->ActiveWaitsForTesting(); }
  bool AcceptingOperations() const { return publisher_->AcceptingOperationsForTesting(); }
  void GateNextOperation() { publisher_->GateNextOperationForTesting(); }
  void WaitForGatedOperation() { publisher_->WaitForGatedOperationForTesting(); }
  void ReleaseGatedOperation() { publisher_->ReleaseGatedOperationForTesting(); }
  std::uint64_t last_sequence() const noexcept { return publisher_->last_sequence(); }
  bool is_exhausted() const noexcept { return publisher_->is_exhausted(); }
  bool may_have_submitted() const noexcept { return publisher_->may_have_submitted(); }

 private:
  friend class FenceTestAccess;
  std::unique_ptr<fence_internal::FencePublisher> publisher_;
};

struct ConsecutiveReservationObservation {
  bool first_admitted = false;
  bool second_admitted = false;
  AckResultV1 through_two;
};

struct DispatchCapacityObservation {
  std::size_t admitted = 0;
  bool overflow_rejected = false;
  std::vector<std::uint64_t> callback_tickets;
};

struct OverflowCase {
  std::string_view kind;
  std::uint64_t sequence = 0;
  std::uint64_t through = 0;
  Status status = Status::Error;
  std::uint64_t failed = kAckNoFailedSequence;
};

class FenceReceiverHarness {
 public:
  Result<void> ObserveExpected(std::uint64_t sequence) {
    lane_.RecordCompleted(sequence);
    return Result<void>::Ok();
  }
  Result<void> ObserveFuture(std::uint64_t sequence) { return ObserveExpected(sequence); }
  Result<void> ObserveStale(std::uint64_t sequence) { return ObserveExpected(sequence); }
  Result<void> ObserveMalformed(const FenceLaneMalformed& malformed) {
    if (malformed.publisher_uuid.has_value()) lane_.RecordMalformed(malformed.sequence);
    return Result<void>::Ok();
  }
  Result<void> ObserveRaw(const FenceUuid& publisher_uuid, std::uint64_t sequence) {
    raw_lanes_[FormatFenceUuid(publisher_uuid)].RecordCompleted(sequence);
    return Result<void>::Ok();
  }
  std::uint64_t CompletedThrough(const FenceUuid& publisher_uuid) const {
    const auto lane = raw_lanes_.find(FormatFenceUuid(publisher_uuid));
    return lane == raw_lanes_.end() ? 0 : lane->second.completed_through();
  }
  AckResultV1 EvaluateMarker(std::uint64_t through) const { return lane_.Evaluate(through); }

  void Reset() {
    lane_.Reset();
    raw_lanes_.clear();
  }
  void ResetRetainedFailureOnly() {
    lane_.Reset();
    lane_.RecordCompleted(1);
  }
  bool RecordOverflow(const OverflowCase& row) {
    if (row.kind == "malformed-uuid-only") {
      lane_.RecordMalformed(std::nullopt);
    } else if (row.kind == "malformed-sequence") {
      lane_.RecordMalformed(row.sequence);
    } else {
      lane_.RecordOverflow(row.sequence);
    }
    return true;
  }

 private:
  fence_internal::FenceLaneState lane_;
  std::unordered_map<std::string, fence_internal::FenceLaneState> raw_lanes_;
};

class FenceTestAccess {
 public:
  static fence_internal::FencePublisherBinding CacheReceiverBinding(
      std::string_view sid, const FenceUuid& receiver_generation) {
    return {fence_internal::FencePublisherTarget::Cache,
            "sitos",
            std::string(sid),
            receiver_generation,
            std::nullopt,
            AckDurability::Applied};
  }

  static fence_internal::FencePublisherBinding BufferReceiverBinding(
      std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
      AckDurability durability) {
    return {fence_internal::FencePublisherTarget::Buffer,
            "sitos",
            std::string(sid),
            session_generation,
            buffer_class,
            durability};
  }

  static FencePublisherHarness CreatePublisher(Transport& transport,
                                               const FenceUuid& publisher_uuid,
                                               fence_internal::FencePublisherBinding binding) {
    return FencePublisherHarness(std::make_unique<fence_internal::FencePublisher>(
        transport, publisher_uuid, std::move(binding)));
  }

  static void CompletePublisherFence(FencePublisherHarness& publisher, const AckToken& token) {
    // The test supplies the receiver's direct cache completion for the exact
    // prepublished token and immutable through value.
    const auto through = publisher.publisher_->PendingThrough(token);
    if (!through.has_value()) return;
    static_cast<void>(publisher.publisher_->Complete(
        token, AckResultV1{AckOperationKind::Fence, Status::Ok, AckDurability::Applied, 0,
                           kAckNoFailedIndex, *through, kAckNoFailedSequence, ""}));
  }

  static bool PublisherWaiterPublished(const FencePublisherHarness& publisher,
                                       const AckToken& token) {
    return publisher.publisher_->WaiterPublished(token);
  }

  static void SetLastSequenceForTesting(FencePublisherHarness& publisher, std::uint64_t sequence) {
    publisher.publisher_->SetLastSequenceForTesting(sequence);
  }

  static bool SetSessionGeneration(StorageNode& node, std::string_view sid,
                                   const FenceUuid& generation);
  static bool BufferValueExists(StorageNode& node, std::string_view sid, std::string_view key);
  static std::optional<AckResultV1> FindAckResult(StorageNode& node, const AckToken& token);
  static std::size_t FenceTokenClaims(StorageNode& node, const AckToken& token);
  static std::size_t BufferApplicationCount(StorageNode& node);
  static std::size_t AckRegistryEntries(StorageNode& node);
  static std::size_t AckRegistryProcessingEntries(StorageNode& node);
  static bool ThrowStorageDispatchOnce(StorageNode& node);
  static bool ThrowCacheDispatchOnce(ParamCache& cache);
  static bool ThrowFenceAfterTokenClaimOnce(StorageNode& node);
  static bool FailFenceCompletionRetentionOnce(StorageNode& node);
  static std::size_t FenceReceiverRegistryEntries(StorageNode& node);
  static std::optional<FenceUuid> VisibleSessionGeneration(StorageNode& node, std::string_view sid);
  static bool BeginClose(StorageNode& node, std::string_view sid);
  static bool CloseSessionFenceDispatch(StorageNode& node, std::string_view sid);
  static bool SetFenceDurabilityBarrier(StorageNode& node,
                                        std::function<Result<void>(StorageEngine&)> barrier);
  static std::size_t FenceDurabilityBarrierCalls(StorageNode& node);
  static bool CloseAndRecreateSession(StorageNode& node, std::string_view sid,
                                      SessionOptions options, const FenceUuid& repeated_generation);

  static TransportSample MakeCoveredBufferPut(std::string_view key, const FenceUuid& publisher_uuid,
                                              std::uint64_t sequence) {
    static constexpr std::array<std::byte, 1> payload{std::byte{1}};
    return {std::string(key),           payload,
            Encoding{"zenoh/bytes"},    AckAttachmentAbsent{},
            TransportSample::Kind::Put, FenceLaneMetadata{publisher_uuid, sequence}};
  }

  static TransportSample MakeBufferMarker(std::string_view prefix, std::string_view sid,
                                          const FenceUuid& session_generation,
                                          BufferClass buffer_class, const FenceUuid& publisher_uuid,
                                          AckDurability durability, std::uint64_t through_sequence,
                                          const AckToken& token) {
    static constexpr std::array<std::byte, 1> marker{std::byte{1}};
    const auto key = BuildBufferFenceMarkerKey(prefix, sid, session_generation, buffer_class,
                                               publisher_uuid, durability, through_sequence);
    return {key.value_or(""),
            marker,
            Encoding{std::string(Encoding::kSitosV1Fence)},
            token,
            TransportSample::Kind::Put,
            FenceLaneAbsent{}};
  }

  static TransportSample MakeMalformedBufferMarker(
      std::string_view prefix, std::string_view sid, const FenceUuid& session_generation,
      BufferClass buffer_class, const FenceUuid& publisher_uuid, AckDurability durability,
      std::uint64_t through_sequence, const AckToken& token) {
    static constexpr std::array<std::byte, 1> malformed_marker{std::byte{2}};
    const auto key = BuildBufferFenceMarkerKey(prefix, sid, session_generation, buffer_class,
                                               publisher_uuid, durability, through_sequence);
    return {key.value_or(""),
            malformed_marker,
            Encoding{std::string(Encoding::kSitosV1Fence)},
            token,
            TransportSample::Kind::Put,
            FenceLaneAbsent{}};
  }

  static TransportSample MakeMalformedBufferMarkerWithoutAck(std::string_view prefix,
                                                             std::string_view sid,
                                                             const FenceUuid& session_generation) {
    static constexpr std::array<std::byte, 1> malformed_marker{std::byte{2}};
    const auto key = BuildBufferFenceMarkerKey(
        prefix, sid, session_generation, BufferClass::Durable,
        FenceUuid{{std::byte{0x8b}, std::byte{0x8f}, std::byte{0x3a}, std::byte{0x62},
                   std::byte{0x7d}, std::byte{0xd5}, std::byte{0x4c}, std::byte{0x40},
                   std::byte{0x8a}, std::byte{0x2b}, std::byte{0x28}, std::byte{0xf7},
                   std::byte{0x13}, std::byte{0x31}, std::byte{0xfe}, std::byte{0x41}}},
        AckDurability::Applied, 0);
    return {key.value_or(""),
            malformed_marker,
            Encoding{std::string(Encoding::kSitosV1Fence)},
            AckAttachmentAbsent{},
            TransportSample::Kind::Put,
            FenceLaneAbsent{}};
  }

  static Result<void> SubmitProductionSyncedFence(Transport& transport, std::string_view sid,
                                                  const FenceUuid& session_generation,
                                                  const FenceUuid& publisher_uuid,
                                                  std::uint64_t through_sequence) {
    fence_internal::FencePublisher publisher(
        transport, publisher_uuid,
        fence_internal::FencePublisherBinding{fence_internal::FencePublisherTarget::Buffer, "sitos",
                                              std::string(sid), session_generation,
                                              BufferClass::Durable, AckDurability::Synced});
    publisher.SetLastSequenceForTesting(through_sequence);
    auto result = publisher.BeginFence(std::chrono::milliseconds(100));
    if (!result.IsOk()) return Result<void>::ErrFrom(result);
    return Result<void>::Ok();
  }

  static bool ConfigureCacheFenceReceiver(ParamCache& cache, const FenceUuid& attach_generation,
                                          const FenceUuid& publisher_uuid);
  static std::uint64_t CacheCompletedThrough(const ParamCache& cache);
  static std::size_t CacheMutationCount(const ParamCache& cache);
  static Result<fence_internal::FenceHandle> BeginCacheFence(ParamCache& cache,
                                                             std::chrono::milliseconds deadline);
  static Result<fence_internal::FenceHandle> PublishCacheWaiterForTesting(
      ParamCache& cache, std::uint64_t through_sequence, std::chrono::milliseconds deadline);
  static Result<fence_internal::FenceHandle> PublishCacheWaiterWithToken(
      ParamCache& cache, std::uint64_t through_sequence, std::chrono::milliseconds deadline,
      const AckToken& token);
  static bool ReceiveCacheMarker(ParamCache& cache, const FenceUuid& attach_generation,
                                 const FenceUuid& publisher_uuid, const AckToken& token);
  static std::optional<AckResultV1> CacheFenceResult(const ParamCache& cache,
                                                     const AckToken& token);
  static AckResultV1 CacheFirstFailure(const ParamCache& cache);
  static bool CacheContains(const ParamCache& cache, std::string_view key);
  static bool CacheFencePending(const ParamCache& cache, const AckToken& token);
  static FenceUuid CacheAttachGeneration(const ParamCache& cache);
  static bool GateCacheCallback(ParamCache& cache);
  static bool WaitForCacheCallbackBlocked(ParamCache& cache);
  static bool WaitUntilCacheAdmissionClosed(ParamCache& cache);
  static bool LateCacheMarkerCanAccessState(ParamCache& cache, const FenceUuid& attach_generation,
                                            const FenceUuid& publisher_uuid, const AckToken& token);
  static bool ReleaseCacheCallback(ParamCache& cache);
  static bool CacheCallbacksQuiesced(const ParamCache& cache);

  static bool GateSessionAfterFenceRegistration(StorageNode& node);
  static bool WaitForSessionFenceRegistration(StorageNode& node);
  static bool ReleaseSessionFenceRegistration(StorageNode& node);
  static bool GateCloseAfterFenceDispatch(StorageNode& node);
  static bool WaitForCloseAfterFenceDispatch(StorageNode& node);
  static bool ReleaseCloseAfterFenceDispatch(StorageNode& node);
  static bool GateClosedMarkerFallback(StorageNode& node);
  static bool WaitForClosedMarkerFallback(StorageNode& node);
  static bool ReleaseClosedMarkerFallback(StorageNode& node);
  static bool GateFenceAfterTokenClaim(StorageNode& node);
  static bool WaitForFenceTokenClaim(StorageNode& node, const AckToken& token);
  static bool ReleaseFenceAfterTokenClaim(StorageNode& node);
  static bool WaitUntilStorageNodeAdmissionClosed(StorageNode& node);
  static std::optional<AckResultV1> LastCompletedFenceResult(StorageNode& node);
  static bool StorageNodeCallbacksQuiesced(StorageNode& node);
  static bool LateNodeCallbackCanAccessState(StorageNode& node);

  static std::optional<AckResultV1> FenceWaiterResult(const fence_internal::FenceHandle& handle) {
    std::scoped_lock lock(handle.waiter->mutex);
    return handle.waiter->result;
  }

  static TransportSample MakeCoveredCachePut(std::string_view key, const FenceUuid& publisher_uuid,
                                             std::uint64_t sequence) {
    static const std::vector<std::byte> payload = ParamValue(std::int64_t{1}).Encode();
    return {std::string(key),
            payload,
            Encoding{std::string(Encoding::kSitosV1)},
            AckAttachmentAbsent{},
            TransportSample::Kind::Put,
            FenceLaneMetadata{publisher_uuid, sequence}};
  }

  static TransportSample MakeCoveredCacheBatch(std::string_view key,
                                               const FenceUuid& publisher_uuid,
                                               std::uint64_t sequence,
                                               std::vector<std::byte>& payload_storage,
                                               std::initializer_list<std::string_view> keys) {
    std::vector<BatchEntry> entries;
    for (const auto item : keys) {
      entries.push_back(BatchEntry{std::string(item), ParamValue(std::int64_t{1})});
    }
    payload_storage = EncodeBatch(entries);
    return {std::string(key),
            payload_storage,
            Encoding{std::string(Encoding::kSitosV1Batch)},
            AckAttachmentAbsent{},
            TransportSample::Kind::Put,
            FenceLaneMetadata{publisher_uuid, sequence}};
  }

  static TransportSample MakeMalformedCachePut(std::string_view key,
                                               const FenceUuid& publisher_uuid,
                                               std::uint64_t sequence) {
    static const std::vector<std::byte> payload = ParamValue(std::int64_t{1}).Encode();
    return {std::string(key),
            payload,
            Encoding{std::string(Encoding::kSitosV1)},
            AckAttachmentAbsent{},
            TransportSample::Kind::Put,
            FenceLaneMalformed{publisher_uuid, sequence}};
  }

  static TransportSample MakeUnidentifiedMalformedParameterPut(std::string_view key) {
    static const std::vector<std::byte> payload = ParamValue(std::int64_t{1}).Encode();
    return {std::string(key),
            payload,
            Encoding{std::string(Encoding::kSitosV1)},
            AckAttachmentAbsent{},
            TransportSample::Kind::Put,
            FenceLaneMalformed{std::nullopt, std::nullopt}};
  }

  static ConsecutiveReservationObservation ExerciseConsecutiveReservations() {
    fence_internal::FenceDispatchCoordinator coordinator(256);
    auto registration = coordinator.Register();
    fence_internal::FenceLaneState lane;
    auto first = coordinator.Dispatch(registration, [&lane] { return lane.Admit(1); }, [] {});
    auto second = coordinator.Dispatch(registration, [&lane] { return lane.Admit(2); }, [] {});
    const bool first_admitted =
        first.outcome == fence_internal::FenceDispatchCoordinator::Outcome::Admitted;
    const bool second_admitted =
        second.outcome == fence_internal::FenceDispatchCoordinator::Outcome::Admitted;
    if (first_admitted) {
      first.entry.WaitTurn();
      lane.Complete(1);
      first.entry = {};
    }
    if (second_admitted) {
      second.entry.WaitTurn();
      lane.Complete(2);
      second.entry = {};
    }
    coordinator.CloseAndWait(registration);
    return {first_admitted, second_admitted, lane.Evaluate(2)};
  }

  static DispatchCapacityObservation ExerciseGlobalDispatchCapacity(
      std::size_t capacity, std::function<void()> overflow_action = {},
      std::optional<std::size_t> worker_count = std::nullopt,
      std::chrono::milliseconds admission_timeout = std::chrono::seconds(5)) {
    fence_internal::FenceDispatchCoordinator coordinator(capacity);
    auto first = coordinator.Register();
    auto second = coordinator.Register();
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool release = false;
    std::vector<std::uint64_t> tickets;
    const auto workers_to_start = worker_count.value_or(capacity);
    std::vector<std::thread> workers;
    workers.reserve(workers_to_start);
    for (std::size_t index = 0; index < workers_to_start; ++index) {
      workers.emplace_back([&, index] {
        const auto& registration = index % 2 == 0 ? first : second;
        auto admission = coordinator.Dispatch(registration, [] { return true; }, [] {});
        if (admission.outcome != fence_internal::FenceDispatchCoordinator::Outcome::Admitted) {
          return;
        }
        admission.entry.WaitTurn();
        {
          std::scoped_lock lock(gate_mutex);
          tickets.push_back(admission.entry.ticket());
          gate_condition.notify_all();
        }
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&release] { return release; });
      });
    }
    const auto admission_deadline = std::chrono::steady_clock::now() + admission_timeout;
    while (coordinator.Admitted() != capacity &&
           std::chrono::steady_clock::now() < admission_deadline) {
      std::this_thread::yield();
    }
    std::size_t overflow_calls = 0;
    auto overflow = coordinator.Dispatch(
        first, [] { return true; },
        [&] {
          ++overflow_calls;
          if (overflow_action) overflow_action();
        });
    const auto admitted = coordinator.Admitted();
    {
      std::scoped_lock lock(gate_mutex);
      release = true;
      gate_condition.notify_all();
    }
    for (auto& worker : workers) worker.join();
    overflow.entry = {};
    coordinator.CloseAndWait(first);
    coordinator.CloseAndWait(second);
    return {admitted,
            overflow.outcome == fence_internal::FenceDispatchCoordinator::Outcome::Overflow &&
                overflow_calls == 1,
            std::move(tickets)};
  }

  static DispatchCapacityObservation ExerciseDispatchCapacity(std::size_t capacity) {
    fence_internal::FenceDispatchLane lane(capacity);
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool release = false;
    std::vector<std::uint64_t> tickets;
    std::vector<std::thread> workers;
    workers.reserve(capacity);
    for (std::size_t index = 0; index < capacity; ++index) {
      static_cast<void>(index);
      workers.emplace_back([&] {
        auto lease = lane.TryEnter();
        if (!lease.has_value()) return;
        lease->WaitTurn();
        {
          std::scoped_lock lock(gate_mutex);
          tickets.push_back(lease->ticket());
          gate_condition.notify_all();
        }
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&release] { return release; });
      });
    }
    const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (lane.Admitted() != capacity && std::chrono::steady_clock::now() < admission_deadline) {
      std::this_thread::yield();
    }
    const auto admitted = lane.Admitted();
    const bool overflow_rejected = !lane.TryEnter().has_value();
    {
      std::scoped_lock lock(gate_mutex);
      release = true;
      gate_condition.notify_all();
    }
    for (auto& worker : workers) worker.join();
    return {admitted, overflow_rejected, std::move(tickets)};
  }

  static FenceReceiverHarness CreateReceiver() { return FenceReceiverHarness{}; }

  static std::uint64_t FenceSequence(const PutOptions& options) noexcept {
    return options.fence_lane.has_value() ? options.fence_lane->sequence : 0;
  }

  static std::array<std::byte, kFenceLaneAttachmentV1Size> EncodeLane(
      const FenceLaneMetadata& metadata) {
    return EncodeFenceLaneAttachment(metadata);
  }

  static FenceLaneObservation ObserveLane(std::span<const std::byte> attachment) noexcept {
    return ObserveFenceLaneAttachment(attachment);
  }

  static std::array<std::byte, kFenceMarkerV1Size> EncodeMarker() noexcept {
    return EncodeFenceMarker();
  }

  static Result<void> DecodeMarker(std::span<const std::byte> payload) {
    return DecodeFenceMarker(payload);
  }

  static fence_internal::AttachmentObservations ClassifyAttachment(
      std::span<const std::byte> attachment) noexcept {
    return fence_internal::ClassifyAttachment(attachment);
  }

  static TransportSample MakeSampleFromPut(std::string_view key, std::span<const std::byte> payload,
                                           Encoding encoding, const PutOptions& options) {
    AckAttachmentObservation ack{AckAttachmentAbsent{}};
    FenceLaneObservation lane{FenceLaneAbsent{}};
    if (options.ack_token.has_value()) ack = *options.ack_token;
    if (options.fence_lane.has_value()) lane = *options.fence_lane;
    return {std::string(key),           payload,        std::move(encoding), std::move(ack),
            TransportSample::Kind::Put, std::move(lane)};
  }

  static TransportSample MakeSample(std::string_view key,
                                    const fence_internal::AttachmentObservations& observations,
                                    TransportSample::Kind kind) {
    static const std::vector<std::byte> parameter_payload = ParamValue(std::int64_t{1}).Encode();
    static const std::array<std::byte, 1> buffer_payload{std::byte{1}};
    const bool buffer = key.find("/buffers/") != std::string_view::npos;
    const auto payload = buffer ? std::span<const std::byte>(buffer_payload)
                                : std::span<const std::byte>(parameter_payload);
    const Encoding encoding{buffer ? "zenoh/bytes" : std::string(Encoding::kSitosV1)};
    return {std::string(key), payload, encoding, observations.ack, kind, observations.lane};
  }

  static TransportSample MakeMarkerFromRoute(std::string_view route, const AckToken& token) {
    static constexpr std::array<std::byte, 1> marker{std::byte{1}};
    return {std::string(route),
            marker,
            Encoding{std::string(Encoding::kSitosV1Fence)},
            token,
            TransportSample::Kind::Put,
            FenceLaneAbsent{}};
  }

  static TransportSample MakeCacheMarkerSample(
      std::string_view prefix, std::string_view sid, const FenceUuid& receiver_generation,
      const FenceUuid& publisher_uuid, std::uint64_t through_sequence,
      const fence_internal::AttachmentObservations& observations) {
    static constexpr std::array<std::byte, 1> marker{std::byte{1}};
    auto key = BuildCacheFenceMarkerKey(prefix, sid, receiver_generation, publisher_uuid,
                                        through_sequence);
    return {key.value_or(""),
            marker,
            Encoding{std::string(Encoding::kSitosV1Fence)},
            observations.ack,
            TransportSample::Kind::Put,
            observations.lane};
  }

  static std::optional<FenceMarkerRoute> ParseMarkerRoute(std::string_view route) {
    return ParseFenceMarkerRoute("sitos", route);
  }

  static bool ParameterWasApplied(StorageNode& node, std::string_view relative_key) {
    std::shared_ptr<StorageNode::State> state;
    {
      std::scoped_lock lock(node.lifecycle_mutex_);
      state = node.state_;
    }
    if (!state) return false;
    std::shared_ptr<StorageNode::SessionRecord> record;
    std::optional<StorageNode::SessionRecord::AdmissionLease> admission;
    {
      std::shared_lock lock(state->session_mutex);
      const auto it = state->sessions.find("s1");
      if (it == state->sessions.end()) return false;
      record = it->second;
      admission = record->TryAcquire();
    }
    if (!admission || !record->overlay) return false;
    return record->overlay->Get(relative_key, [](std::string_view, Bytes) { return true; });
  }
};

}  // namespace sitos::fence_test_access

#endif  // SITOS_FENCE_TEST_ACCESS_HPP
