// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <system_error>
#include <vector>

#include "ack_client.hpp"
#include "fence_internal.hpp"
#include "sitos/ack.hpp"

namespace sitos {
using namespace fence_internal;
namespace {

constexpr std::byte kFenceSchemaVersion{1};

class ActiveOperationGuard {
 public:
  ActiveOperationGuard(std::mutex& mutex, std::condition_variable& condition,
                       std::size_t& active_operations)
      : mutex_(mutex), condition_(condition), active_operations_(active_operations) {}
  ~ActiveOperationGuard() {
    std::scoped_lock lock(mutex_);
    assert(active_operations_ > 0);
    --active_operations_;
    condition_.notify_all();
  }
  ActiveOperationGuard(const ActiveOperationGuard&) = delete;
  ActiveOperationGuard& operator=(const ActiveOperationGuard&) = delete;

 private:
  std::mutex& mutex_;
  std::condition_variable& condition_;
  std::size_t& active_operations_;
};

std::uint64_t ReadSequence(std::span<const std::byte> bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index]))
             << (index * 8);
  }
  return value;
}

std::optional<std::uint64_t> ParseLooseSequence(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> ParseCanonicalSequence(std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<FenceUuid> ParseLooseFenceUuid(std::string_view text) {
  std::string canonical(text);
  for (char& character : canonical) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return ParseFenceUuid(canonical);
}

std::vector<std::string_view> SplitRoute(std::string_view text, bool allow_trailing_slash = false) {
  std::vector<std::string_view> chunks;
  if (text.empty() || text.front() == '/' || (!allow_trailing_slash && text.back() == '/')) {
    return chunks;
  }
  if (allow_trailing_slash && text.back() == '/') text.remove_suffix(1);
  while (!text.empty()) {
    const auto slash = text.find('/');
    const auto chunk = text.substr(0, slash);
    if (chunk.empty()) return {};
    chunks.push_back(chunk);
    if (slash == std::string_view::npos) break;
    text.remove_prefix(slash + 1);
  }
  return chunks;
}

std::optional<std::string_view> StripFencePrefix(std::string_view prefix,
                                                 std::string_view full_key) {
  if (!IsValidPrefix(prefix) || full_key.size() <= prefix.size() || !full_key.starts_with(prefix) ||
      full_key[prefix.size()] != '/') {
    return std::nullopt;
  }
  return full_key.substr(prefix.size() + 1);
}

std::string BufferClassName(BufferClass buffer_class) {
  return buffer_class == BufferClass::Durable ? "durable" : "ephemeral";
}

std::string DurabilityName(AckDurability durability) {
  return durability == AckDurability::Applied ? "applied" : "synced";
}

}  // namespace

bool fence_internal::IsValidFenceUuid(const FenceUuid& uuid) noexcept {
  return IsValidAckToken(AckToken{uuid});
}

FenceUuid fence_internal::GenerateFenceUuid() { return GenerateAckToken().bytes; }

std::string fence_internal::FormatFenceUuid(const FenceUuid& uuid) {
  return FormatAckToken(AckToken{uuid});
}

std::optional<FenceUuid> fence_internal::ParseFenceUuid(std::string_view text) {
  auto token = ParseAckToken(text);
  if (!token.has_value()) return std::nullopt;
  return token->bytes;
}

std::array<std::byte, fence_internal::kFenceLaneAttachmentV1Size>
fence_internal::EncodeFenceLaneAttachment(const FenceLaneMetadata& metadata) {
  assert(IsValidFenceUuid(metadata.publisher_uuid));
  assert(metadata.sequence != 0);
  std::array<std::byte, kFenceLaneAttachmentV1Size> bytes{};
  bytes[0] = kFenceSchemaVersion;
  for (std::size_t index = 0; index != metadata.publisher_uuid.size(); ++index) {
    bytes[1 + index] = metadata.publisher_uuid[index];
  }
  for (std::size_t index = 0; index != 8; ++index) {
    bytes[17 + index] = static_cast<std::byte>((metadata.sequence >> (index * 8)) & 0xffU);
  }
  return bytes;
}

FenceLaneObservation fence_internal::ObserveFenceLaneAttachment(
    std::span<const std::byte> attachment) noexcept {
  const bool exact_size = attachment.size() == kFenceLaneAttachmentV1Size;
  const bool version_one_candidate =
      !attachment.empty() && attachment.front() == kFenceSchemaVersion;
  if (!exact_size && !version_one_candidate) return FenceLaneAbsent{};

  std::optional<FenceUuid> publisher_uuid;
  if (attachment.size() >= 17) {
    FenceUuid candidate{};
    for (std::size_t index = 0; index != candidate.size(); ++index) {
      candidate[index] = attachment[1 + index];
    }
    if (IsValidFenceUuid(candidate)) publisher_uuid = candidate;
  }

  std::optional<std::uint64_t> sequence;
  if (version_one_candidate && attachment.size() >= kFenceLaneAttachmentV1Size) {
    const std::uint64_t candidate = ReadSequence(attachment.subspan(17, 8));
    if (candidate != 0) sequence = candidate;
  }

  if (exact_size && version_one_candidate && publisher_uuid.has_value() && sequence.has_value()) {
    return FenceLaneMetadata{*publisher_uuid, *sequence};
  }
  return FenceLaneMalformed{publisher_uuid, sequence};
}

std::array<std::byte, fence_internal::kFenceMarkerV1Size>
fence_internal::EncodeFenceMarker() noexcept {
  return {kFenceSchemaVersion};
}

Result<void> fence_internal::DecodeFenceMarker(std::span<const std::byte> payload) {
  if (payload.size() != kFenceMarkerV1Size) {
    return Result<void>::Err(Status::InvalidArgument,
                             "FenceMarkerV1 payload must be exactly one byte");
  }
  if (payload.front() != kFenceSchemaVersion) {
    return Result<void>::Err(Status::InvalidArgument, "FenceMarkerV1 schema_version is unknown");
  }
  return Result<void>::Ok();
}

std::optional<std::string> fence_internal::BuildCacheFenceMarkerKey(
    std::string_view prefix, std::string_view sid, const FenceUuid& receiver_generation,
    const FenceUuid& publisher_uuid, std::uint64_t through_sequence) {
  if (!IsValidPrefix(prefix) || !IsValidSessionId(sid) || !IsValidFenceUuid(receiver_generation) ||
      !IsValidFenceUuid(publisher_uuid)) {
    return std::nullopt;
  }
  return std::string(prefix) + "/meta/fence/cache/" + std::string(sid) + "/" +
         FormatFenceUuid(receiver_generation) + "/" + FormatFenceUuid(publisher_uuid) + "/" +
         std::to_string(through_sequence);
}

std::optional<std::string> fence_internal::BuildBufferFenceMarkerKey(
    std::string_view prefix, std::string_view sid, const FenceUuid& session_generation,
    BufferClass buffer_class, const FenceUuid& publisher_uuid, AckDurability durability,
    std::uint64_t through_sequence) {
  if (!IsValidPrefix(prefix) || !IsValidSessionId(sid) || !IsValidFenceUuid(session_generation) ||
      !IsValidFenceUuid(publisher_uuid)) {
    return std::nullopt;
  }
  if (buffer_class != BufferClass::Durable && buffer_class != BufferClass::Ephemeral) {
    return std::nullopt;
  }
  if (durability != AckDurability::Applied && durability != AckDurability::Synced) {
    return std::nullopt;
  }
  if (buffer_class == BufferClass::Ephemeral && durability == AckDurability::Synced) {
    return std::nullopt;
  }
  return std::string(prefix) + "/meta/fence/buffer/" + std::string(sid) + "/" +
         FormatFenceUuid(session_generation) + "/" + BufferClassName(buffer_class) + "/" +
         FormatFenceUuid(publisher_uuid) + "/" + DurabilityName(durability) + "/" +
         std::to_string(through_sequence);
}

struct fence_internal::FenceDispatchLane::State {
  explicit State(std::size_t limit) : capacity(limit) {}

  std::mutex mutex;
  std::condition_variable condition;
  const std::size_t capacity;
  std::uint64_t next_ticket = 0;
  std::uint64_t serving_ticket = 0;
  std::size_t admitted = 0;
  bool accepting = true;
};

fence_internal::FenceDispatchLane::Lease::Lease(std::shared_ptr<State> state, std::uint64_t ticket)
    : state_(std::move(state)), ticket_(ticket) {}

fence_internal::FenceDispatchLane::Lease::~Lease() { Release(); }

fence_internal::FenceDispatchLane::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), ticket_(other.ticket_) {}

fence_internal::FenceDispatchLane::Lease& fence_internal::FenceDispatchLane::Lease::operator=(
    Lease&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::move(other.state_);
    ticket_ = other.ticket_;
  }
  return *this;
}

void fence_internal::FenceDispatchLane::Lease::WaitTurn() {
  if (!state_) return;
  std::unique_lock lock(state_->mutex);
  state_->condition.wait(lock, [this] { return ticket_ == state_->serving_ticket; });
}

void fence_internal::FenceDispatchLane::Lease::Release() noexcept {
  if (!state_) return;
  std::scoped_lock lock(state_->mutex);
  assert(ticket_ == state_->serving_ticket);
  ++state_->serving_ticket;
  assert(state_->admitted > 0);
  --state_->admitted;
  state_->condition.notify_all();
  state_.reset();
}

fence_internal::FenceDispatchLane::FenceDispatchLane(std::size_t capacity)
    : state_(std::make_shared<State>(capacity)) {}

std::optional<fence_internal::FenceDispatchLane::Lease>
fence_internal::FenceDispatchLane::TryEnter() {
  std::uint64_t ticket = 0;
  {
    std::scoped_lock lock(state_->mutex);
    if (!state_->accepting || state_->admitted == state_->capacity) return std::nullopt;
    ticket = state_->next_ticket++;
    ++state_->admitted;
  }
  return Lease(state_, ticket);
}

void fence_internal::FenceDispatchLane::CloseAndWait() {
  std::unique_lock lock(state_->mutex);
  state_->accepting = false;
  state_->condition.notify_all();
  state_->condition.wait(lock, [this] { return state_->admitted == 0; });
}

std::size_t fence_internal::FenceDispatchLane::Admitted() const {
  std::scoped_lock lock(state_->mutex);
  return state_->admitted;
}

bool fence_internal::FenceDispatchLane::HasCapacity() const {
  std::scoped_lock lock(state_->mutex);
  return state_->accepting && state_->admitted < state_->capacity;
}

bool fence_internal::FenceDispatchLane::IsAccepting() const {
  std::scoped_lock lock(state_->mutex);
  return state_->accepting;
}

struct fence_internal::FenceDispatchCoordinator::RegistrationState {
  std::mutex mutex;
  std::condition_variable condition;
  bool accepting = true;
  std::size_t admitted = 0;
};

fence_internal::FenceDispatchCoordinator::Entry::Entry(
    FenceDispatchLane::Lease lease, std::shared_ptr<RegistrationState> registration)
    : lease_(std::move(lease)), registration_(std::move(registration)) {}

fence_internal::FenceDispatchCoordinator::Entry::~Entry() { Release(); }

fence_internal::FenceDispatchCoordinator::Entry::Entry(Entry&& other) noexcept
    : lease_(std::move(other.lease_)), registration_(std::move(other.registration_)) {}

fence_internal::FenceDispatchCoordinator::Entry&
fence_internal::FenceDispatchCoordinator::Entry::operator=(Entry&& other) noexcept {
  if (this != &other) {
    Release();
    lease_ = std::move(other.lease_);
    registration_ = std::move(other.registration_);
  }
  return *this;
}

void fence_internal::FenceDispatchCoordinator::Entry::WaitTurn() {
  if (lease_.has_value()) lease_->WaitTurn();
}

void fence_internal::FenceDispatchCoordinator::Entry::Release() noexcept {
  if (!lease_.has_value()) return;
  lease_.reset();
  if (registration_) {
    std::scoped_lock lock(registration_->mutex);
    assert(registration_->admitted > 0);
    --registration_->admitted;
    registration_->condition.notify_all();
    registration_.reset();
  }
}

fence_internal::FenceDispatchCoordinator::Registration
fence_internal::FenceDispatchCoordinator::Register() {
  return Registration(std::make_shared<RegistrationState>());
}

fence_internal::FenceDispatchCoordinator::Admission
fence_internal::FenceDispatchCoordinator::Dispatch(const Registration& registration,
                                                   const std::function<bool()>& prepare,
                                                   const std::function<void()>& overflow) {
  if (!registration.state_) return {};
  std::scoped_lock admission_lock(admission_mutex_);
  {
    std::scoped_lock registration_lock(registration.state_->mutex);
    if (!registration.state_->accepting) return {Outcome::Closed, {}};
  }
  if (!lane_.HasCapacity()) {
    if (overflow) overflow();
    return {Outcome::Overflow, {}};
  }
  if (prepare && !prepare()) return {Outcome::Rejected, {}};
  auto lease = lane_.TryEnter();
  if (!lease.has_value()) return {Outcome::Overflow, {}};
  {
    std::scoped_lock registration_lock(registration.state_->mutex);
    ++registration.state_->admitted;
  }
  return {Outcome::Admitted, Entry(std::move(*lease), registration.state_)};
}

void fence_internal::FenceDispatchCoordinator::CloseAndWait(Registration& registration) {
  if (!registration.state_) return;
  {
    std::scoped_lock admission_lock(admission_mutex_);
    std::scoped_lock registration_lock(registration.state_->mutex);
    registration.state_->accepting = false;
  }
  std::unique_lock lock(registration.state_->mutex);
  registration.state_->condition.wait(
      lock, [&registration] { return registration.state_->admitted == 0; });
}

void fence_internal::FenceLaneState::Latch(Status status, std::uint64_t sequence) {
  if (!first_failure_.has_value()) first_failure_ = FenceFirstFailure{status, sequence};
}

bool fence_internal::FenceLaneState::Admit(std::uint64_t sequence) {
  if (sequence == 0) {
    Latch(Status::Error, kAckNoFailedSequence);
    return false;
  }
  if (sequence > highest_observed_) highest_observed_ = sequence;
  if (reservation_exhausted_ || sequence <= reserved_through_) {
    Latch(Status::Error, sequence);
    return false;
  }
  const std::uint64_t expected = reserved_through_ + 1;
  if (sequence > expected) {
    Latch(Status::OutcomeUnknown, expected);
    return false;
  }
  reserved_through_ = sequence;
  if (sequence == UINT64_MAX) reservation_exhausted_ = true;
  return true;
}

void fence_internal::FenceLaneState::Complete(std::uint64_t sequence,
                                              std::optional<Status> failure) {
  const std::uint64_t expected = completed_through_ + 1;
  if (sequence != expected || sequence > reserved_through_) {
    Latch(Status::Error, sequence == 0 ? kAckNoFailedSequence : sequence);
    return;
  }
  completed_through_ = sequence;
  if (failure.has_value() && *failure != Status::Ok) Latch(*failure, sequence);
}

void fence_internal::FenceLaneState::RecordCompleted(std::uint64_t sequence,
                                                     std::optional<Status> failure) {
  if (Admit(sequence)) Complete(sequence, failure);
}

void fence_internal::FenceLaneState::RecordRejected(std::uint64_t observed_sequence, Status status,
                                                    std::uint64_t failed_sequence) {
  if (observed_sequence > highest_observed_) highest_observed_ = observed_sequence;
  Latch(status, failed_sequence);
}

void fence_internal::FenceLaneState::RecordOverflow(std::uint64_t observed_sequence) {
  if (observed_sequence > highest_observed_) highest_observed_ = observed_sequence;
  if (reservation_exhausted_ || observed_sequence <= reserved_through_) {
    Latch(Status::Error, observed_sequence);
  } else {
    Latch(Status::OutcomeUnknown, reserved_through_ + 1);
  }
}

void fence_internal::FenceLaneState::RecordMalformed(std::optional<std::uint64_t> sequence) {
  if (sequence.has_value()) {
    if (*sequence > highest_observed_) highest_observed_ = *sequence;
    Latch(Status::Error, *sequence);
    return;
  }
  Latch(Status::Error, kAckNoFailedSequence);
}

AckResultV1 fence_internal::FenceLaneState::Evaluate(std::uint64_t through_sequence,
                                                     AckDurability durability) const {
  auto result = AckResultV1{AckOperationKind::Fence, Status::Ok,       durability,           0,
                            kAckNoFailedIndex,       through_sequence, kAckNoFailedSequence, ""};
  if (first_failure_.has_value() && first_failure_->sequence == kAckNoFailedSequence) {
    result.status = first_failure_->status;
    return result;
  }
  if (first_failure_.has_value() && first_failure_->sequence <= through_sequence) {
    result.status = first_failure_->status;
    result.failed_sequence = first_failure_->sequence;
    return result;
  }
  if (completed_through_ > through_sequence || highest_observed_ > through_sequence) {
    result.status = Status::OutcomeUnknown;
    return result;
  }
  if (completed_through_ < through_sequence) {
    result.status = Status::OutcomeUnknown;
    result.failed_sequence = completed_through_ + 1;
  }
  return result;
}

void fence_internal::FenceLaneState::Reset() noexcept {
  completed_through_ = 0;
  highest_observed_ = 0;
  reserved_through_ = 0;
  reservation_exhausted_ = false;
  first_failure_.reset();
}

void fence_internal::FenceLaneState::SetCompletedForTesting(std::uint64_t sequence) noexcept {
  completed_through_ = sequence;
  highest_observed_ = sequence;
  reserved_through_ = sequence;
  reservation_exhausted_ = sequence == UINT64_MAX;
  first_failure_.reset();
}

std::string fence_internal::FenceReceiverRegistry::LaneKey(std::string_view sid,
                                                           const FenceUuid& session_generation,
                                                           BufferClass buffer_class,
                                                           const FenceUuid& publisher_uuid) {
  return std::string(sid) + "|" + FormatFenceUuid(session_generation) + "|" +
         BufferClassName(buffer_class) + "|" + FormatFenceUuid(publisher_uuid);
}

std::string fence_internal::FenceReceiverRegistry::ScopeKey(std::string_view sid,
                                                            const FenceUuid& session_generation,
                                                            BufferClass buffer_class) {
  return std::string(sid) + "|" + FormatFenceUuid(session_generation) + "|" +
         BufferClassName(buffer_class);
}

bool fence_internal::FenceReceiverRegistry::AdmitBufferObservation(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceUuid& publisher_uuid, std::uint64_t sequence) {
  std::scoped_lock lock(mutex_);
  const auto key = LaneKey(sid, session_generation, buffer_class, publisher_uuid);
  auto lane = lanes_.find(key);
  if (lane == lanes_.end()) {
    if (lanes_.size() == capacity_) {
      poisoned_scopes_[ScopeKey(sid, session_generation, buffer_class)] = true;
      return false;
    }
    lane = lanes_.try_emplace(key).first;
  }
  return lane->second.Admit(sequence);
}

void fence_internal::FenceReceiverRegistry::CompleteBufferObservation(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceUuid& publisher_uuid, std::uint64_t sequence, std::optional<Status> failure) {
  std::scoped_lock lock(mutex_);
  const auto lane = lanes_.find(LaneKey(sid, session_generation, buffer_class, publisher_uuid));
  if (lane != lanes_.end()) lane->second.Complete(sequence, failure);
}

bool fence_internal::FenceReceiverRegistry::RecordBufferObservation(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceUuid& publisher_uuid, std::uint64_t sequence, std::optional<Status> failure) {
  if (!AdmitBufferObservation(sid, session_generation, buffer_class, publisher_uuid, sequence)) {
    return false;
  }
  CompleteBufferObservation(sid, session_generation, buffer_class, publisher_uuid, sequence,
                            failure);
  return true;
}

bool fence_internal::FenceReceiverRegistry::RecordMalformedBuffer(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceLaneMalformed& malformed) {
  if (!malformed.publisher_uuid.has_value()) return false;
  std::scoped_lock lock(mutex_);
  const auto key = LaneKey(sid, session_generation, buffer_class, *malformed.publisher_uuid);
  auto lane = lanes_.find(key);
  if (lane == lanes_.end()) {
    if (lanes_.size() == capacity_) {
      poisoned_scopes_[ScopeKey(sid, session_generation, buffer_class)] = true;
      return false;
    }
    lane = lanes_.try_emplace(key).first;
  }
  lane->second.RecordMalformed(malformed.sequence);
  return true;
}

bool fence_internal::FenceReceiverRegistry::RecordBufferOverflow(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceUuid& publisher_uuid, std::uint64_t sequence) {
  std::scoped_lock lock(mutex_);
  const auto key = LaneKey(sid, session_generation, buffer_class, publisher_uuid);
  auto lane = lanes_.find(key);
  if (lane == lanes_.end()) {
    if (lanes_.size() == capacity_) {
      poisoned_scopes_[ScopeKey(sid, session_generation, buffer_class)] = true;
      return false;
    }
    lane = lanes_.try_emplace(key).first;
  }
  lane->second.RecordOverflow(sequence);
  return true;
}

AckResultV1 fence_internal::FenceReceiverRegistry::EvaluateBuffer(
    std::string_view sid, const FenceUuid& session_generation, BufferClass buffer_class,
    const FenceUuid& publisher_uuid, AckDurability durability,
    std::uint64_t through_sequence) const {
  std::scoped_lock lock(mutex_);
  const auto lane = lanes_.find(LaneKey(sid, session_generation, buffer_class, publisher_uuid));
  if (lane != lanes_.end()) return lane->second.Evaluate(through_sequence, durability);
  AckResultV1 result{AckOperationKind::Fence, Status::Ok,       durability,           0,
                     kAckNoFailedIndex,       through_sequence, kAckNoFailedSequence, ""};
  if (through_sequence > 0) {
    result.status = Status::OutcomeUnknown;
    result.failed_sequence = 1;
    return result;
  }
  const auto poison = poisoned_scopes_.find(ScopeKey(sid, session_generation, buffer_class));
  if (poison != poisoned_scopes_.end() && poison->second) {
    result.status = Status::OutcomeUnknown;
  }
  return result;
}

void fence_internal::FenceReceiverRegistry::EraseSession(std::string_view sid) noexcept {
  const auto belongs_to_session = [sid](const auto& item) {
    const auto& key = item.first;
    return key.size() > sid.size() && key.compare(0, sid.size(), sid) == 0 &&
           key[sid.size()] == '|';
  };
  std::scoped_lock lock(mutex_);
  std::erase_if(lanes_, belongs_to_session);
  std::erase_if(poisoned_scopes_, belongs_to_session);
}

void fence_internal::FenceReceiverRegistry::Clear() {
  std::scoped_lock lock(mutex_);
  lanes_.clear();
  poisoned_scopes_.clear();
}

std::size_t fence_internal::FenceReceiverRegistry::Size() const {
  std::scoped_lock lock(mutex_);
  return lanes_.size();
}

fence_internal::FencePublisher::FencePublisher(Transport& transport, FenceUuid publisher_uuid,
                                               FencePublisherBinding binding)
    : transport_(&transport),
      publisher_uuid_(publisher_uuid),
      binding_(std::move(binding)),
      transport_generation_(transport.FenceGeneration()) {}

fence_internal::FencePublisher::~FencePublisher() { Close(); }

bool fence_internal::FencePublisher::AdmitOperation() {
  std::scoped_lock lock(wait_lifecycle_mutex_);
  if (!accepting_operations_) return false;
  ++active_operations_;
  return true;
}

void fence_internal::FencePublisher::WaitAtOperationGateForTesting() {
  std::unique_lock lock(operation_test_mutex_);
  if (!operation_test_gate_enabled_) return;
  operation_test_gate_enabled_ = false;
  operation_test_gate_entered_ = true;
  operation_test_condition_.notify_all();
  operation_test_condition_.wait(lock, [this] { return operation_test_gate_released_; });
}

void fence_internal::FencePublisher::QuiescePeerOperations() {
  std::unique_lock lock(wait_lifecycle_mutex_);
  if (operation_quiescer_active_) return;
  operation_quiescer_active_ = true;
  wait_lifecycle_condition_.wait(lock, [this] { return active_operations_ <= 1; });
  operation_quiescer_active_ = false;
  wait_lifecycle_condition_.notify_all();
}

bool fence_internal::FencePublisher::CheckGeneration() {
  if (disconnected_) return false;
  if (transport_->FenceGeneration() == transport_generation_) return true;
  Disconnect();
  return false;
}

void fence_internal::FencePublisher::Disconnect() {
  disconnected_ = true;
  {
    std::scoped_lock lifecycle_lock(wait_lifecycle_mutex_);
    accepting_operations_ = false;
  }
  std::optional<FenceHandle> pending;
  {
    std::scoped_lock lock(waiter_mutex_);
    pending = pending_;
  }
  if (!pending.has_value()) return;
  auto result =
      AckResultV1{AckOperationKind::Fence, Status::Disconnected,      binding_.durability,  0,
                  kAckNoFailedIndex,       pending->through_sequence, kAckNoFailedSequence, ""};
  static_cast<void>(Complete(pending->token, std::move(result)));
}

Result<void> fence_internal::FencePublisher::SubmitData(std::string_view key,
                                                        std::span<const std::byte> payload,
                                                        Encoding encoding) {
  if (!AdmitOperation()) return Result<void>::Err(Status::Disconnected);
  ActiveOperationGuard operation(wait_lifecycle_mutex_, wait_lifecycle_condition_,
                                 active_operations_);
  WaitAtOperationGateForTesting();
  std::unique_lock lane_lock(lane_mutex_);
  if (!CheckGeneration()) {
    lane_lock.unlock();
    QuiescePeerOperations();
    return Result<void>::Err(Status::Disconnected);
  }
  if (!transport_->SupportsFenceProfile()) {
    return Result<void>::Err(Status::InvalidArgument, "Transport does not support Fence");
  }
  if (exhausted_) return Result<void>::Err(Status::InvalidArgument, "Fence sequence exhausted");

  ++last_sequence_;
  if (last_sequence_ == UINT64_MAX) exhausted_ = true;
  PutOptions options;
  options.fence_lane = FenceLaneMetadata{publisher_uuid_, last_sequence_};
  auto result = transport_->Put(key, payload, std::move(encoding), std::move(options));
  if (!result.IsOk()) {
    may_have_submitted_ = true;
    latest_submission_error_ =
        ErrorInfo{result.StatusCode(), std::string(result.Message()), result.Error()};
  }
  return result;
}

Result<void> fence_internal::FencePublisher::SubmitForTesting(std::string_view operation) {
  static constexpr std::array<std::byte, 1> payload{std::byte{1}};
  std::string key;
  Encoding encoding;
  if (operation == "batch") {
    key = binding_.prefix + "/session/" + binding_.sid + "/:batch";
    encoding.id = std::string(Encoding::kSitosV1Batch);
  } else if (operation == "push") {
    const auto buffer_class = binding_.buffer_class.value_or(BufferClass::Ephemeral);
    key = BuildBufferKey(binding_.prefix, binding_.sid, buffer_class, "value").value_or("");
    encoding.id = "zenoh/bytes";
  } else {
    key = binding_.prefix + "/session/" + binding_.sid + "/value";
    encoding.id = std::string(Encoding::kSitosV1);
  }
  return SubmitData(key, payload, std::move(encoding));
}

Result<fence_internal::FenceHandle> fence_internal::FencePublisher::BeginFence(
    std::chrono::milliseconds total_deadline) {
  if (!AdmitOperation()) return Result<FenceHandle>::Err(Status::Disconnected);
  ActiveOperationGuard operation(wait_lifecycle_mutex_, wait_lifecycle_condition_,
                                 active_operations_);
  WaitAtOperationGateForTesting();
  std::unique_lock lane_lock(lane_mutex_);
  if (!CheckGeneration()) {
    lane_lock.unlock();
    QuiescePeerOperations();
    return Result<FenceHandle>::Err(Status::Disconnected);
  }
  if (total_deadline.count() <= 0) {
    return Result<FenceHandle>::Err(Status::InvalidArgument, "Fence deadline must be positive");
  }
  if (!transport_->SupportsFenceProfile()) {
    return Result<FenceHandle>::Err(Status::InvalidArgument, "Transport does not support Fence");
  }
  if (binding_.target == FencePublisherTarget::Buffer &&
      binding_.durability == AckDurability::Synced) {
    return Result<FenceHandle>::Err(Status::InvalidArgument,
                                    "synchronized Fence requires the #105 barrier");
  }
  {
    std::scoped_lock waiter_lock(waiter_mutex_);
    if (pending_.has_value()) {
      return Result<FenceHandle>::Err(Status::InvalidArgument, "Fence already pending",
                                      std::make_error_code(std::errc::operation_in_progress));
    }
  }

  const std::uint64_t through_sequence = last_sequence_;
  std::optional<std::string> key;
  if (binding_.target == FencePublisherTarget::Cache) {
    key = BuildCacheFenceMarkerKey(binding_.prefix, binding_.sid, binding_.receiver_generation,
                                   publisher_uuid_, through_sequence);
  } else if (binding_.buffer_class.has_value()) {
    key = BuildBufferFenceMarkerKey(binding_.prefix, binding_.sid, binding_.receiver_generation,
                                    *binding_.buffer_class, publisher_uuid_, binding_.durability,
                                    through_sequence);
  }
  if (!key.has_value()) {
    return Result<FenceHandle>::Err(Status::InvalidArgument, "invalid Fence binding");
  }

  FenceHandle handle{
      GenerateAckToken(), through_sequence, {}, std::make_shared<FenceWaiterState>()};
  {
    std::scoped_lock waiter_lock(waiter_mutex_);
    pending_ = handle;  // token/waiter/through are visible before synchronous loopback
  }
  PutOptions options;
  options.ack_token = handle.token;
  const auto payload = EncodeFenceMarker();
  handle.deadline = std::chrono::steady_clock::now() + total_deadline;
  {
    std::scoped_lock waiter_lock(waiter_mutex_);
    if (pending_.has_value() && pending_->token == handle.token) {
      pending_->deadline = handle.deadline;
    }
  }
  const auto result = transport_->Put(*key, payload, Encoding{std::string(Encoding::kSitosV1Fence)},
                                      std::move(options));
  if (!result.IsOk()) {
    may_have_submitted_ = true;
    latest_submission_error_ =
        ErrorInfo{result.StatusCode(), std::string(result.Message()), result.Error()};
  }
  return Result<FenceHandle>::Ok(std::move(handle));
}

Result<fence_internal::FenceHandle> fence_internal::FencePublisher::PublishWaiterForTesting(
    std::uint64_t through_sequence, std::chrono::milliseconds total_deadline,
    std::optional<AckToken> fixed_token) {
  if (total_deadline.count() <= 0) {
    return Result<FenceHandle>::Err(Status::InvalidArgument);
  }
  std::scoped_lock lock(waiter_mutex_);
  if (pending_.has_value()) {
    return Result<FenceHandle>::Err(Status::InvalidArgument, "Fence already pending",
                                    std::make_error_code(std::errc::operation_in_progress));
  }
  FenceHandle handle{fixed_token.value_or(GenerateAckToken()), through_sequence,
                     std::chrono::steady_clock::now() + total_deadline,
                     std::make_shared<FenceWaiterState>()};
  pending_ = handle;
  return Result<FenceHandle>::Ok(std::move(handle));
}

Result<AckResultV1> fence_internal::FencePublisher::Wait(const FenceHandle& handle) {
  {
    std::scoped_lock waiter_lock(handle.waiter->mutex);
    if (handle.waiter->terminal && handle.waiter->result.has_value()) {
      return Result<AckResultV1>::Ok(*handle.waiter->result);
    }
  }
  if (!AdmitOperation()) {
    AckResultV1 retained{
        AckOperationKind::Fence, Status::Disconnected,    binding_.durability,  0,
        kAckNoFailedIndex,       handle.through_sequence, kAckNoFailedSequence, ""};
    {
      std::scoped_lock waiter_lock(handle.waiter->mutex);
      if (handle.waiter->terminal && handle.waiter->result.has_value()) {
        retained = *handle.waiter->result;
      } else if (!handle.waiter->terminal) {
        handle.waiter->result = retained;
        handle.waiter->terminal = true;
      }
    }
    {
      std::scoped_lock lock(waiter_mutex_);
      if (pending_.has_value() && pending_->token == handle.token) pending_.reset();
    }
    handle.waiter->condition.notify_all();
    return Result<AckResultV1>::Ok(std::move(retained));
  }
  ActiveOperationGuard operation(wait_lifecycle_mutex_, wait_lifecycle_condition_,
                                 active_operations_);
  std::optional<ErrorInfo> latest_error;
  bool generation_current = false;
  {
    std::scoped_lock lane_lock(lane_mutex_);
    generation_current = CheckGeneration();
    latest_error = latest_submission_error_;
  }
  if (!generation_current) QuiescePeerOperations();
  if (binding_.target == FencePublisherTarget::Buffer) {
    auto result = PollAcknowledgement(
        *transport_, binding_.prefix, handle.token, handle.deadline, std::move(latest_error),
        [waiter = handle.waiter]() -> std::optional<AckResultV1> {
          std::scoped_lock lock(waiter->mutex);
          return waiter->terminal ? waiter->result : std::nullopt;
        },
        [waiter = handle.waiter](const AckResultV1& observed) {
          {
            std::scoped_lock lock(waiter->mutex);
            if (waiter->terminal) return;
            waiter->result = observed;
            waiter->terminal = true;
          }
          waiter->condition.notify_all();
        });
    if (result.IsOk()) {
      std::scoped_lock waiter_lock(handle.waiter->mutex);
      if (!handle.waiter->terminal) {
        handle.waiter->result = result.Value();
        handle.waiter->terminal = true;
      } else if (handle.waiter->result.has_value()) {
        result = Result<AckResultV1>::Ok(*handle.waiter->result);
      }
    } else {
      std::scoped_lock waiter_lock(handle.waiter->mutex);
      if (handle.waiter->terminal && handle.waiter->result.has_value()) {
        result = Result<AckResultV1>::Ok(*handle.waiter->result);
      }
    }
    std::scoped_lock lock(waiter_mutex_);
    if (pending_.has_value() && pending_->token == handle.token) pending_.reset();
    return result;
  }
  std::optional<AckResultV1> result;
  {
    std::unique_lock lock(handle.waiter->mutex);
    if (!handle.waiter->condition.wait_until(lock, handle.deadline,
                                             [&handle] { return handle.waiter->terminal; })) {
      handle.waiter->terminal = true;  // timeout wins atomically against completion
    }
    result = handle.waiter->result;
  }
  {
    std::scoped_lock lock(waiter_mutex_);
    if (pending_.has_value() && pending_->token == handle.token) pending_.reset();
  }
  if (!result.has_value()) {
    std::scoped_lock lane_lock(lane_mutex_);
    if (latest_error.has_value()) {
      return Result<AckResultV1>::Err(Status::Timeout, latest_error->message, latest_error->cause);
    }
    return Result<AckResultV1>::Err(Status::Timeout);
  }
  return Result<AckResultV1>::Ok(std::move(*result));
}

bool fence_internal::FencePublisher::Complete(const AckToken& token, AckResultV1 result) {
  std::shared_ptr<FenceWaiterState> waiter;
  {
    std::scoped_lock lock(waiter_mutex_);
    if (!pending_.has_value() || pending_->token != token) return false;
    waiter = pending_->waiter;
    std::scoped_lock waiter_lock(waiter->mutex);
    if (waiter->terminal) return false;
    waiter->result = std::move(result);
    waiter->terminal = true;
    pending_.reset();
  }
  waiter->condition.notify_all();
  return true;
}

void fence_internal::FencePublisher::Close() {
  {
    std::scoped_lock lifecycle_lock(wait_lifecycle_mutex_);
    accepting_operations_ = false;
  }
  {
    std::scoped_lock lane_lock(lane_mutex_);
    Disconnect();
  }
  std::unique_lock lifecycle_lock(wait_lifecycle_mutex_);
  wait_lifecycle_condition_.wait(lifecycle_lock, [this] { return active_operations_ == 0; });
}

void fence_internal::FencePublisher::SetLastSequenceForTesting(std::uint64_t sequence) noexcept {
  std::scoped_lock lock(lane_mutex_);
  last_sequence_ = sequence;
  exhausted_ = sequence == UINT64_MAX;
}

std::uint64_t fence_internal::FencePublisher::last_sequence() const noexcept {
  std::scoped_lock lock(lane_mutex_);
  return last_sequence_;
}

bool fence_internal::FencePublisher::is_exhausted() const noexcept {
  std::scoped_lock lock(lane_mutex_);
  return exhausted_;
}

bool fence_internal::FencePublisher::may_have_submitted() const noexcept {
  std::scoped_lock lock(lane_mutex_);
  return may_have_submitted_;
}

bool fence_internal::FencePublisher::WaiterPublished(const AckToken& token) const {
  std::scoped_lock lock(waiter_mutex_);
  return pending_.has_value() && pending_->token == token;
}

std::optional<std::uint64_t> fence_internal::FencePublisher::PendingThrough(
    const AckToken& token) const {
  std::scoped_lock lock(waiter_mutex_);
  if (!pending_.has_value() || pending_->token != token) return std::nullopt;
  return pending_->through_sequence;
}

std::size_t fence_internal::FencePublisher::ActiveWaitsForTesting() const {
  std::scoped_lock lock(wait_lifecycle_mutex_);
  return active_operations_;
}

bool fence_internal::FencePublisher::AcceptingOperationsForTesting() const {
  std::scoped_lock lock(wait_lifecycle_mutex_);
  return accepting_operations_;
}

void fence_internal::FencePublisher::GateNextOperationForTesting() {
  std::scoped_lock lock(operation_test_mutex_);
  operation_test_gate_enabled_ = true;
  operation_test_gate_entered_ = false;
  operation_test_gate_released_ = false;
}

void fence_internal::FencePublisher::WaitForGatedOperationForTesting() {
  std::unique_lock lock(operation_test_mutex_);
  operation_test_condition_.wait(lock, [this] { return operation_test_gate_entered_; });
}

void fence_internal::FencePublisher::ReleaseGatedOperationForTesting() {
  std::scoped_lock lock(operation_test_mutex_);
  operation_test_gate_released_ = true;
  operation_test_condition_.notify_all();
}

fence_internal::AttachmentObservations fence_internal::ClassifyAttachment(
    std::optional<std::span<const std::byte>> attachment) noexcept {
  AttachmentObservations observations;
  if (!attachment.has_value()) return observations;
  const auto bytes = *attachment;
  if (bytes.size() == kAckAttachmentV1Size) {
    observations.ack = ObserveAckAttachment(attachment);
    return observations;
  }
  if (bytes.size() == kFenceLaneAttachmentV1Size ||
      (!bytes.empty() && bytes.front() == kFenceSchemaVersion)) {
    observations.lane = ObserveFenceLaneAttachment(bytes);
  }
  return observations;
}

std::optional<FenceMarkerRoute> fence_internal::RecoverMalformedFenceMarkerRoute(
    std::string_view prefix, std::string_view full_key) {
  auto remainder = StripFencePrefix(prefix, full_key);
  if (!remainder.has_value()) return std::nullopt;
  const auto chunks = SplitRoute(*remainder, /*allow_trailing_slash=*/true);
  if (chunks.size() < 4 || chunks[0] != "meta" || chunks[1] != "fence") {
    return std::nullopt;
  }
  if (chunks[2] == "cache") {
    if (chunks.size() != 7 || !IsValidSessionId(chunks[3])) return std::nullopt;
    const auto receiver = ParseLooseFenceUuid(chunks[4]);
    const auto publisher = ParseLooseFenceUuid(chunks[5]);
    const auto through = ParseLooseSequence(chunks[6]);
    if (!receiver || !publisher || !through) return std::nullopt;
    return FenceMarkerRoute{FenceMarkerTarget::Cache,
                            std::string(chunks[3]),
                            *receiver,
                            std::nullopt,
                            *publisher,
                            AckDurability::Applied,
                            *through};
  }
  if (chunks[2] == "buffer") {
    if (chunks.size() != 9 || !IsValidSessionId(chunks[3])) return std::nullopt;
    const auto session = ParseLooseFenceUuid(chunks[4]);
    const auto publisher = ParseLooseFenceUuid(chunks[6]);
    const auto through = ParseLooseSequence(chunks[8]);
    if (!session || !publisher || !through) return std::nullopt;
    std::optional<BufferClass> buffer_class;
    if (chunks[5] == "durable") buffer_class = BufferClass::Durable;
    if (chunks[5] == "ephemeral") buffer_class = BufferClass::Ephemeral;
    std::optional<AckDurability> durability;
    if (chunks[7] == "applied") durability = AckDurability::Applied;
    if (chunks[7] == "synced") durability = AckDurability::Synced;
    if (!buffer_class || !durability) return std::nullopt;
    return FenceMarkerRoute{FenceMarkerTarget::Buffer,
                            std::string(chunks[3]),
                            *session,
                            *buffer_class,
                            *publisher,
                            *durability,
                            *through};
  }
  return std::nullopt;
}

std::optional<fence_internal::FenceMarkerRoute> fence_internal::ParseFenceMarkerRoute(
    std::string_view prefix, std::string_view full_key) {
  auto remainder = StripFencePrefix(prefix, full_key);
  if (!remainder.has_value()) return std::nullopt;
  const auto chunks = SplitRoute(*remainder);
  if (chunks.size() < 4 || chunks[0] != "meta" || chunks[1] != "fence") {
    return std::nullopt;
  }
  if (chunks[2] == "cache") {
    if (chunks.size() != 7 || !IsValidSessionId(chunks[3])) return std::nullopt;
    const auto receiver = ParseFenceUuid(chunks[4]);
    const auto publisher = ParseFenceUuid(chunks[5]);
    const auto through = ParseCanonicalSequence(chunks[6]);
    if (!receiver || !publisher || !through) return std::nullopt;
    return FenceMarkerRoute{FenceMarkerTarget::Cache,
                            std::string(chunks[3]),
                            *receiver,
                            std::nullopt,
                            *publisher,
                            AckDurability::Applied,
                            *through};
  }
  if (chunks[2] == "buffer") {
    if (chunks.size() != 9 || !IsValidSessionId(chunks[3])) return std::nullopt;
    const auto session = ParseFenceUuid(chunks[4]);
    const auto publisher = ParseFenceUuid(chunks[6]);
    const auto through = ParseCanonicalSequence(chunks[8]);
    if (!session || !publisher || !through) return std::nullopt;
    std::optional<BufferClass> buffer_class;
    if (chunks[5] == "durable") buffer_class = BufferClass::Durable;
    if (chunks[5] == "ephemeral") buffer_class = BufferClass::Ephemeral;
    std::optional<AckDurability> durability;
    if (chunks[7] == "applied") durability = AckDurability::Applied;
    if (chunks[7] == "synced") durability = AckDurability::Synced;
    if (!buffer_class || !durability ||
        (*buffer_class == BufferClass::Ephemeral && *durability == AckDurability::Synced)) {
      return std::nullopt;
    }
    return FenceMarkerRoute{FenceMarkerTarget::Buffer,
                            std::string(chunks[3]),
                            *session,
                            *buffer_class,
                            *publisher,
                            *durability,
                            *through};
  }
  return std::nullopt;
}

}  // namespace sitos
