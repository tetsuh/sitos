// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_FENCE_INTERNAL_HPP
#define SITOS_FENCE_INTERNAL_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "sitos/ack.hpp"
#include "sitos/key.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos::fence_internal {

inline constexpr std::size_t kFenceLaneAttachmentV1Size = 25;
inline constexpr std::size_t kFenceMarkerV1Size = 1;

[[nodiscard]] bool IsValidFenceUuid(const FenceUuid& uuid) noexcept;
[[nodiscard]] FenceUuid GenerateFenceUuid();
[[nodiscard]] std::string FormatFenceUuid(const FenceUuid& uuid);
[[nodiscard]] std::optional<FenceUuid> ParseFenceUuid(std::string_view text);
[[nodiscard]] std::array<std::byte, kFenceLaneAttachmentV1Size> EncodeFenceLaneAttachment(
    const FenceLaneMetadata& metadata);
[[nodiscard]] FenceLaneObservation ObserveFenceLaneAttachment(
    std::span<const std::byte> attachment) noexcept;
[[nodiscard]] std::array<std::byte, kFenceMarkerV1Size> EncodeFenceMarker() noexcept;
[[nodiscard]] Result<void> DecodeFenceMarker(std::span<const std::byte> payload);

enum class FenceMarkerTarget { Cache, Buffer };

struct FenceMarkerRoute {
  FenceMarkerTarget target = FenceMarkerTarget::Cache;
  std::string sid;
  FenceUuid receiver_generation{};
  std::optional<BufferClass> buffer_class;
  FenceUuid publisher_uuid{};
  AckDurability durability = AckDurability::Applied;
  std::uint64_t through_sequence = 0;

  bool operator==(const FenceMarkerRoute&) const = default;
};

[[nodiscard]] std::optional<std::string> BuildCacheFenceMarkerKey(
    std::string_view prefix, std::string_view sid, const FenceUuid& receiver_generation,
    const FenceUuid& publisher_uuid, std::uint64_t through_sequence);
[[nodiscard]] std::optional<std::string> BuildBufferFenceMarkerKey(
    std::string_view prefix, std::string_view sid, const FenceUuid& session_generation,
    BufferClass buffer_class, const FenceUuid& publisher_uuid, AckDurability durability,
    std::uint64_t through_sequence);
[[nodiscard]] std::optional<FenceMarkerRoute> ParseFenceMarkerRoute(std::string_view prefix,
                                                                    std::string_view full_key);

struct AttachmentObservations {
  AckAttachmentObservation ack{AckAttachmentAbsent{}};
  FenceLaneObservation lane{FenceLaneAbsent{}};
};

/// Disjoint ADR-0028/ADR-0029/raw attachment classification.
[[nodiscard]] AttachmentObservations ClassifyAttachment(
    std::optional<std::span<const std::byte>> attachment) noexcept;

/// Recovers all marker fields from a reserved route whose canonical spelling or
/// target/class combination is invalid. Used only to create protocol Error.
[[nodiscard]] std::optional<FenceMarkerRoute> RecoverMalformedFenceMarkerRoute(
    std::string_view prefix, std::string_view full_key);

struct FenceFirstFailure {
  Status status = Status::Error;
  std::uint64_t sequence = kAckNoFailedSequence;
};

/// O(1) ADR-0029 proof retained for one target/Publisher lane.
class FenceLaneState {
 public:
  /// Reserves the exact next sequence before receiver application. Rejections
  /// latch proof and must not be applied.
  [[nodiscard]] bool Admit(std::uint64_t sequence);
  void Complete(std::uint64_t sequence, std::optional<Status> failure = std::nullopt);
  void RecordCompleted(std::uint64_t sequence, std::optional<Status> failure = std::nullopt);
  void RecordRejected(std::uint64_t observed_sequence, Status status,
                      std::uint64_t failed_sequence);
  void RecordOverflow(std::uint64_t observed_sequence);
  void RecordMalformed(std::optional<std::uint64_t> sequence);
  [[nodiscard]] AckResultV1 Evaluate(std::uint64_t through_sequence,
                                     AckDurability durability = AckDurability::Applied) const;
  void Reset() noexcept;
  void SetCompletedForTesting(std::uint64_t sequence) noexcept;

  [[nodiscard]] std::uint64_t completed_through() const noexcept { return completed_through_; }
  [[nodiscard]] std::uint64_t highest_observed() const noexcept { return highest_observed_; }
  [[nodiscard]] std::uint64_t next_expected() const noexcept {
    return reservation_exhausted_ ? UINT64_MAX : reserved_through_ + 1;
  }

 private:
  void Latch(Status status, std::uint64_t sequence);

  std::uint64_t completed_through_ = 0;
  std::uint64_t highest_observed_ = 0;
  std::uint64_t reserved_through_ = 0;
  bool reservation_exhausted_ = false;
  std::optional<FenceFirstFailure> first_failure_;
};

enum class FencePublisherTarget { Cache, Buffer };

struct FencePublisherBinding {
  FencePublisherTarget target = FencePublisherTarget::Cache;
  std::string prefix;
  std::string sid;
  FenceUuid receiver_generation{};
  std::optional<BufferClass> buffer_class;
  AckDurability durability = AckDurability::Applied;
};

struct FenceWaiterState {
  std::mutex mutex;
  std::condition_variable condition;
  bool terminal = false;
  bool completed_on_bound_generation = false;
  std::optional<AckResultV1> result;
  /// Monotonic instant at which the terminal result linearized. ADR-0029 makes a
  /// completion successful only when it precedes the handle deadline, so a
  /// synchronous callback that runs after the deadline while the marker Put is
  /// still executing cannot later be reported as success.
  std::optional<std::chrono::steady_clock::time_point> completed_at;
};

struct FenceHandle {
  AckToken token;
  std::uint64_t through_sequence = 0;
  std::chrono::steady_clock::time_point deadline;
  std::shared_ptr<FenceWaiterState> waiter;
};

/// Internal ADR-0029 logical Publisher lane used by later #99/#107 surfaces.
class FenceDispatchLane {
 private:
  struct State;

 public:
  static constexpr std::size_t kDefaultCapacity = 256;

  class Lease {
   public:
    Lease() = default;
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    explicit operator bool() const noexcept { return state_ != nullptr; }
    void WaitTurn();
    [[nodiscard]] std::uint64_t ticket() const noexcept { return ticket_; }

   private:
    friend class FenceDispatchLane;
    Lease(std::shared_ptr<State> state, std::uint64_t ticket);
    void Release() noexcept;
    std::shared_ptr<State> state_;
    std::uint64_t ticket_ = 0;
  };

  explicit FenceDispatchLane(std::size_t capacity = kDefaultCapacity);
  [[nodiscard]] std::optional<Lease> TryEnter();
  void CloseAndWait();
  [[nodiscard]] std::size_t Admitted() const;
  [[nodiscard]] bool HasCapacity() const;
  [[nodiscard]] bool IsAccepting() const;

 private:
  std::shared_ptr<State> state_;
};

class FenceDispatchCoordinator {
 private:
  struct RegistrationState;

 public:
  class Registration {
   public:
    Registration() = default;

   private:
    friend class FenceDispatchCoordinator;
    explicit Registration(std::shared_ptr<RegistrationState> state) : state_(std::move(state)) {}
    std::shared_ptr<RegistrationState> state_;
  };

  class Entry {
   public:
    Entry() = default;
    ~Entry();
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&& other) noexcept;
    Entry& operator=(Entry&& other) noexcept;
    explicit operator bool() const noexcept { return lease_.has_value(); }
    void WaitTurn();
    [[nodiscard]] std::uint64_t ticket() const noexcept {
      return lease_.has_value() ? lease_->ticket() : 0;
    }

   private:
    friend class FenceDispatchCoordinator;
    Entry(FenceDispatchLane::Lease lease, std::shared_ptr<RegistrationState> registration);
    void Release() noexcept;
    std::optional<FenceDispatchLane::Lease> lease_;
    std::shared_ptr<RegistrationState> registration_;
  };

  enum class Outcome { Admitted, Rejected, Overflow, Closed };
  struct Admission {
    Outcome outcome = Outcome::Closed;
    Entry entry;
  };

  explicit FenceDispatchCoordinator(std::size_t capacity = 256) : lane_(capacity) {}
  [[nodiscard]] Registration Register();
  [[nodiscard]] Admission Dispatch(const Registration& registration,
                                   const std::function<bool()>& prepare,
                                   const std::function<void()>& overflow);
  void CloseAndWait(Registration& registration);
  [[nodiscard]] std::size_t Admitted() const { return lane_.Admitted(); }

 private:
  std::mutex admission_mutex_;
  FenceDispatchLane lane_;
};

struct FenceSessionDispatch {
  FenceDispatchCoordinator::Registration durable;
  FenceDispatchCoordinator::Registration ephemeral;
};

class FenceReceiverRegistry {
 public:
  static constexpr std::size_t kDefaultCapacity = 4096;

  explicit FenceReceiverRegistry(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

  [[nodiscard]] bool AdmitBufferObservation(std::string_view sid,
                                            const FenceUuid& session_generation,
                                            BufferClass buffer_class,
                                            const FenceUuid& publisher_uuid,
                                            std::uint64_t sequence);
  void CompleteBufferObservation(std::string_view sid, const FenceUuid& session_generation,
                                 BufferClass buffer_class, const FenceUuid& publisher_uuid,
                                 std::uint64_t sequence,
                                 std::optional<Status> failure = std::nullopt);
  bool RecordBufferObservation(std::string_view sid, const FenceUuid& session_generation,
                               BufferClass buffer_class, const FenceUuid& publisher_uuid,
                               std::uint64_t sequence,
                               std::optional<Status> failure = std::nullopt);
  bool RecordMalformedBuffer(std::string_view sid, const FenceUuid& session_generation,
                             BufferClass buffer_class, const FenceLaneMalformed& malformed);
  bool RecordBufferOverflow(std::string_view sid, const FenceUuid& session_generation,
                            BufferClass buffer_class, const FenceUuid& publisher_uuid,
                            std::uint64_t sequence);
  [[nodiscard]] AckResultV1 EvaluateBuffer(std::string_view sid,
                                           const FenceUuid& session_generation,
                                           BufferClass buffer_class,
                                           const FenceUuid& publisher_uuid,
                                           AckDurability durability,
                                           std::uint64_t through_sequence) const;
  void EraseSession(std::string_view sid) noexcept;
  void Clear();
  [[nodiscard]] std::size_t Size() const;

 private:
  static std::string LaneKey(std::string_view sid, const FenceUuid& session_generation,
                             BufferClass buffer_class, const FenceUuid& publisher_uuid);
  static std::string ScopeKey(std::string_view sid, const FenceUuid& session_generation,
                              BufferClass buffer_class);

  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::unordered_map<std::string, FenceLaneState> lanes_;
  std::unordered_map<std::string, bool> poisoned_scopes_;
};

class FencePublisher {
 public:
  FencePublisher(Transport& transport, FenceUuid publisher_uuid, FencePublisherBinding binding);
  ~FencePublisher();
  FencePublisher(const FencePublisher&) = delete;
  FencePublisher& operator=(const FencePublisher&) = delete;
  FencePublisher(FencePublisher&&) = delete;
  FencePublisher& operator=(FencePublisher&&) = delete;

  [[nodiscard]] Result<void> SubmitData(std::string_view key, std::span<const std::byte> payload,
                                        Encoding encoding);
  [[nodiscard]] Result<void> SubmitForTesting(std::string_view operation);
  [[nodiscard]] Result<FenceHandle> BeginFence(std::chrono::milliseconds total_deadline);
  [[nodiscard]] Result<FenceHandle> PublishWaiterForTesting(
      std::uint64_t through_sequence, std::chrono::milliseconds total_deadline,
      std::optional<AckToken> fixed_token = std::nullopt);
  [[nodiscard]] Result<AckResultV1> Wait(const FenceHandle& handle);
  bool Complete(const AckToken& token, AckResultV1 result);
  void Close();

  void SetLastSequenceForTesting(std::uint64_t sequence) noexcept;
  [[nodiscard]] std::uint64_t last_sequence() const noexcept;
  [[nodiscard]] bool is_exhausted() const noexcept;
  [[nodiscard]] bool may_have_submitted() const noexcept;
  [[nodiscard]] bool WaiterPublished(const AckToken& token) const;
  [[nodiscard]] std::optional<std::uint64_t> PendingThrough(const AckToken& token) const;
  [[nodiscard]] std::size_t ActiveWaitsForTesting() const;
  [[nodiscard]] bool AcceptingOperationsForTesting() const;
  void GateNextOperationForTesting();
  void WaitForGatedOperationForTesting();
  void ReleaseGatedOperationForTesting();

 private:
  bool AdmitOperation();
  void WaitAtOperationGateForTesting();
  void QuiescePeerOperations();
  bool CheckGeneration();
  void MarkGenerationMismatch();
  [[nodiscard]] AckResultV1 DisconnectedResult(std::uint64_t through_sequence) const;
  [[nodiscard]] std::optional<bool> PublishWaiterResult(
      const std::shared_ptr<FenceWaiterState>& waiter, std::uint64_t through_sequence,
      AckResultV1 result);
  void Disconnect();

  Transport* transport_;
  FenceUuid publisher_uuid_;
  FencePublisherBinding binding_;
  const std::uint64_t transport_generation_;
  mutable std::mutex lane_mutex_;
  mutable std::mutex wait_lifecycle_mutex_;
  std::condition_variable wait_lifecycle_condition_;
  std::size_t active_operations_ = 0;
  bool accepting_operations_ = true;
  bool operation_quiescer_active_ = false;
  mutable std::mutex operation_test_mutex_;
  std::condition_variable operation_test_condition_;
  bool operation_test_gate_enabled_ = false;
  bool operation_test_gate_entered_ = false;
  bool operation_test_gate_released_ = false;
  mutable std::mutex waiter_mutex_;
  std::uint64_t last_sequence_ = 0;
  bool exhausted_ = false;
  bool may_have_submitted_ = false;
  std::optional<ErrorInfo> latest_submission_error_;
  std::atomic<bool> generation_mismatch_{false};
  bool disconnected_ = false;
  std::optional<FenceHandle> pending_;
};

}  // namespace sitos::fence_internal

#endif  // SITOS_FENCE_INTERNAL_HPP
