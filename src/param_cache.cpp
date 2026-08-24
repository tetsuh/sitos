// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fence_internal.hpp"
#include "fence_test_access.hpp"
#include "param_cache_test_access.hpp"
#include "sitos/batch.hpp"
#include "sitos/key.hpp"

namespace sitos {
using namespace fence_internal;
namespace {

std::string ScopeQuery(const ClientConfig& config, std::string_view scope) {
  return config.prefix + "/" + std::string(scope) + "/**";
}

Result<void> InvalidArgument(std::string_view message) {
  return Result<void>::Err(Status::InvalidArgument, std::string(message));
}

Result<void> InvalidKey(std::string_view message) {
  return Result<void>::Err(Status::InvalidKey, std::string(message));
}

struct TransparentStringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
  std::size_t operator()(const std::string& value) const noexcept {
    return operator()(std::string_view(value));
  }
};

struct TransparentStringEqual {
  using is_transparent = void;
  bool operator()(std::string_view left, std::string_view right) const noexcept {
    return left == right;
  }
  bool operator()(const std::string& left, const std::string& right) const noexcept {
    return left == right;
  }
  bool operator()(const std::string& left, std::string_view right) const noexcept {
    return left == right;
  }
  bool operator()(std::string_view left, const std::string& right) const noexcept {
    return left == right;
  }
};

}  // namespace

struct ParamCache::Impl {
  using ValueMap = std::unordered_map<std::string, std::shared_ptr<const ParamValue>,
                                      TransparentStringHash, TransparentStringEqual>;
  enum class Phase { Buffering, Live, Stopping };
  enum class MutationKind { Put, Delete };

  struct Mutation {
    MutationKind kind;
    std::string key;
    std::shared_ptr<const ParamValue> value;
  };

  struct State {
    explicit State(std::string prefix_value, std::string sid_value)
        : prefix(std::move(prefix_value)), sid(std::move(sid_value)) {}

    std::string prefix;
    std::string sid;
    Phase phase = Phase::Buffering;
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool accepting = true;
    std::size_t in_flight = 0;
    std::mutex sequence_mutex;
    mutable std::shared_mutex map_mutex;
    ValueMap snapshot_baseline;
    ValueMap effective_map;
    std::vector<Mutation> buffered;
    std::vector<FenceLaneObservation> buffered_fence_observations;
    FenceUuid attach_generation = GenerateFenceUuid();
    FenceUuid publisher_uuid = GenerateFenceUuid();
    fence_internal::FenceLaneState fence_receiver;
    std::shared_ptr<fence_internal::FenceDispatchCoordinator> fence_dispatcher;
    fence_internal::FenceDispatchCoordinator::Registration fence_registration;
    std::unique_ptr<fence_internal::FencePublisher> fence_publisher;
    std::mutex fence_test_mutex;
    std::condition_variable fence_test_condition;
    bool fence_test_callback_blocked = false;
    bool fence_test_callback_released = false;
    bool fence_test_throw_dispatch_once = false;
    std::optional<fence_internal::FenceHandle> last_fence_handle;
    std::function<void()> callback_hook;
    std::function<void()> read_state_hook;
    std::function<void(std::size_t)> mutation_hook;
    std::size_t mutation_count = 0;
    std::size_t callback_mutation_count = 0;
  };

  explicit Impl(std::shared_ptr<Transport> transport_value, ClientConfig config_value)
      : transport(std::move(transport_value)), config(std::move(config_value)) {}

  std::shared_ptr<Transport> transport;
  ClientConfig config;
  std::mutex lifecycle_mutex;
  std::atomic<std::shared_ptr<State>> active_state;
  Subscription subscription;
  Subscription marker_subscription;
};

namespace param_cache_detail {
struct Access {
  using Impl = ParamCache::Impl;
};
}  // namespace param_cache_detail

namespace {

class StateLease {
 public:
  explicit StateLease(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state)
      : state_(state) {
    std::lock_guard lock(state_->gate_mutex);
    if (!state_->accepting) return;
    ++state_->in_flight;
    entered_ = true;
  }

  StateLease(const StateLease&) = delete;
  StateLease& operator=(const StateLease&) = delete;
  StateLease(StateLease&&) = delete;
  StateLease& operator=(StateLease&&) = delete;

  ~StateLease() {
    if (!entered_) return;
    std::lock_guard lock(state_->gate_mutex);
    --state_->in_flight;
    if (state_->in_flight == 0) state_->gate_condition.notify_all();
  }

  explicit operator bool() const noexcept { return entered_; }

 private:
  std::shared_ptr<param_cache_detail::Access::Impl::State> state_;
  bool entered_ = false;
};

using OperationLease = StateLease;
using CallbackLease = StateLease;

std::shared_ptr<param_cache_detail::Access::Impl::State> LoadState(
    const param_cache_detail::Access::Impl& impl) {
  return impl.active_state.load(std::memory_order_acquire);
}

void StoreState(param_cache_detail::Access::Impl& impl,
                const std::shared_ptr<param_cache_detail::Access::Impl::State>& state) {
  impl.active_state.store(state, std::memory_order_release);
}

Result<void> ValidateCacheKey(std::string_view key) {
  if (!IsValidKey(key)) return InvalidKey("invalid cache key");
  return Result<void>::Ok();
}

bool MatchesPrefix(std::string_view key, std::string_view prefix) {
  return prefix.empty() || key.starts_with(prefix);
}

void CloseGate(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state) {
  std::lock_guard lock(state->gate_mutex);
  state->accepting = false;
  state->gate_condition.notify_all();
}

void WaitForCallbacks(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state) {
  std::unique_lock lock(state->gate_mutex);
  state->gate_condition.wait(lock, [state] { return state->in_flight == 0; });
}

void NotifyReadStateLoaded(const param_cache_detail::Access::Impl::State& state) {
  if (state.read_state_hook) state.read_state_hook();
}

bool IsExpected(const ParsedKey& parsed, const param_cache_detail::Access::Impl::State& state,
                bool batch_allowed) {
  return parsed.is_batch == batch_allowed && parsed.kind == KeyKind::Session &&
         parsed.sid == state.sid;
}

std::optional<param_cache_detail::Access::Impl::Mutation> DecodeOrdinary(
    const param_cache_detail::Access::Impl::State& state, const TransportSample& sample) {
  const auto parsed = ParseKey(state.prefix, sample.key);
  if (!parsed || !IsExpected(*parsed, state, false)) return std::nullopt;
  if (sample.kind == TransportSample::Kind::Delete) {
    return param_cache_detail::Access::Impl::Mutation{
        param_cache_detail::Access::Impl::MutationKind::Delete, parsed->relative_key, nullptr};
  }
  if (sample.encoding.id == Encoding::kSitosV1Batch) return std::nullopt;

  std::optional<ParamValue> decoded;
  if (sample.encoding.id == Encoding::kSitosV1) {
    decoded = ParamValue::Decode(sample.payload);
    if (!decoded.has_value()) return std::nullopt;
  } else {
    decoded = ParamValue(std::vector<std::byte>(sample.payload.begin(), sample.payload.end()));
  }
  return param_cache_detail::Access::Impl::Mutation{
      param_cache_detail::Access::Impl::MutationKind::Put, parsed->relative_key,
      std::make_shared<const ParamValue>(std::move(*decoded))};
}

std::optional<std::vector<param_cache_detail::Access::Impl::Mutation>> DecodeSample(
    const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
    const TransportSample& sample) {
  if (sample.kind == TransportSample::Kind::Delete) {
    auto mutation = DecodeOrdinary(*state, sample);
    if (!mutation.has_value()) return std::nullopt;
    return std::vector<param_cache_detail::Access::Impl::Mutation>{std::move(*mutation)};
  }

  const auto parsed = ParseKey(state->prefix, sample.key);
  if (!parsed) return std::nullopt;
  if (parsed->is_batch) {
    if (!IsExpected(*parsed, *state, true) || sample.encoding.id != Encoding::kSitosV1Batch) {
      return std::nullopt;
    }
    auto entries = DecodeBatch(sample.payload);
    if (!entries.has_value()) return std::nullopt;
    std::vector<param_cache_detail::Access::Impl::Mutation> mutations;
    mutations.reserve(entries->size());
    for (auto& entry : *entries) {
      if (!IsValidKey(entry.key)) return std::nullopt;
      mutations.push_back(param_cache_detail::Access::Impl::Mutation{
          param_cache_detail::Access::Impl::MutationKind::Put, std::move(entry.key),
          std::make_shared<const ParamValue>(std::move(entry.value))});
    }
    return mutations;
  }
  auto ordinary = DecodeOrdinary(*state, sample);
  if (!ordinary.has_value()) return std::nullopt;
  return std::vector<param_cache_detail::Access::Impl::Mutation>{std::move(*ordinary)};
}

void ApplyMutation(param_cache_detail::Access::Impl::State& state,
                   const param_cache_detail::Access::Impl::Mutation& mutation) {
  std::unique_lock lock(state.map_mutex);
  if (mutation.kind == param_cache_detail::Access::Impl::MutationKind::Put) {
    state.effective_map[mutation.key] = mutation.value;
    return;
  }
  const auto baseline = state.snapshot_baseline.find(mutation.key);
  if (baseline != state.snapshot_baseline.end()) {
    state.effective_map[mutation.key] = baseline->second;
  } else {
    state.effective_map.erase(mutation.key);
  }
}

void ApplyMutations(param_cache_detail::Access::Impl::State& state,
                    const std::vector<param_cache_detail::Access::Impl::Mutation>& mutations,
                    bool* application_started = nullptr) {
  for (const auto& mutation : mutations) {
    ApplyMutation(state, mutation);
    if (application_started != nullptr) *application_started = true;
    ++state.mutation_count;
    if (state.mutation_hook) state.mutation_hook(state.mutation_count);
  }
}

enum class CacheFenceAdmission { Bypass, Apply, Reject };

CacheFenceAdmission AdmitFenceObservation(param_cache_detail::Access::Impl::State& state,
                                          const FenceLaneObservation& observation,
                                          bool processing_valid) {
  if (const auto* lane = std::get_if<FenceLaneMetadata>(&observation)) {
    if (lane->publisher_uuid != state.publisher_uuid) return CacheFenceAdmission::Bypass;
    if (!state.fence_receiver.Admit(lane->sequence)) return CacheFenceAdmission::Reject;
    if (!processing_valid) {
      state.fence_receiver.Complete(lane->sequence, Status::Error);
      return CacheFenceAdmission::Reject;
    }
    return CacheFenceAdmission::Apply;
  }
  if (const auto* malformed = std::get_if<FenceLaneMalformed>(&observation);
      malformed && malformed->publisher_uuid == state.publisher_uuid) {
    state.fence_receiver.RecordMalformed(malformed->sequence);
    return CacheFenceAdmission::Reject;
  }
  return CacheFenceAdmission::Bypass;
}

void CompleteFenceObservation(param_cache_detail::Access::Impl::State& state,
                              const FenceLaneObservation& observation) {
  if (const auto* lane = std::get_if<FenceLaneMetadata>(&observation);
      lane && lane->publisher_uuid == state.publisher_uuid) {
    state.fence_receiver.Complete(lane->sequence);
  }
}

void OnSample(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
              const TransportSample& sample, bool fence_preadmitted,
              bool* application_started = nullptr) {
  CallbackLease lease(state);
  if (!lease) return;
  if (state->callback_hook) state->callback_hook();
  auto marker = ParseFenceMarkerRoute(state->prefix, sample.key);
  bool malformed_marker_route = false;
  if (!marker.has_value()) {
    marker = fence_internal::RecoverMalformedFenceMarkerRoute(state->prefix, sample.key);
    malformed_marker_route = marker.has_value();
  }
  if (marker && marker->target == FenceMarkerTarget::Cache) {
    if (marker->sid != state->sid || marker->receiver_generation != state->attach_generation ||
        marker->publisher_uuid != state->publisher_uuid) {
      return;
    }
    const auto* token = std::get_if<AckToken>(&sample.ack);
    if (token == nullptr || !state->fence_publisher) return;
    AckResultV1 result{AckOperationKind::Fence, Status::Error,
                       AckDurability::Applied,  0,
                       kAckNoFailedIndex,       marker->through_sequence,
                       kAckNoFailedSequence,    ""};
    if (!malformed_marker_route && sample.kind == TransportSample::Kind::Put &&
        sample.encoding.id == Encoding::kSitosV1Fence && DecodeFenceMarker(sample.payload).IsOk()) {
      std::scoped_lock sequence_lock(state->sequence_mutex);
      result = state->fence_receiver.Evaluate(marker->through_sequence);
    }
    static_cast<void>(state->fence_publisher->Complete(*token, std::move(result)));
    return;
  }
  auto mutations = DecodeSample(state, sample);

  std::lock_guard sequence_lock(state->sequence_mutex);
  if (fence_preadmitted && !mutations.has_value()) {
    if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane);
        lane && lane->publisher_uuid == state->publisher_uuid) {
      state->fence_receiver.Complete(lane->sequence, Status::Error);
    }
    return;
  }
  const auto fence_admission =
      fence_preadmitted ? CacheFenceAdmission::Apply
                        : AdmitFenceObservation(*state, sample.fence_lane, mutations.has_value());
  if (fence_admission == CacheFenceAdmission::Reject || !mutations.has_value()) return;
  if (state->phase == param_cache_detail::Access::Impl::Phase::Buffering) {
    for (auto& mutation : *mutations) {
      state->buffered.push_back(std::move(mutation));
      if (application_started != nullptr) *application_started = true;
    }
    if (fence_admission == CacheFenceAdmission::Apply) {
      state->buffered_fence_observations.push_back(sample.fence_lane);
    }
    return;
  }
  if (state->phase != param_cache_detail::Access::Impl::Phase::Live) return;
  ApplyMutations(*state, *mutations, application_started);
  state->callback_mutation_count += mutations->size();
  if (fence_admission == CacheFenceAdmission::Apply) {
    CompleteFenceObservation(*state, sample.fence_lane);
  }
}

void RetainCacheDispatchFailure(
    const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
    const TransportSample& sample, bool callback_admitted, bool application_started) noexcept {
  const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane);
  const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane);
  const bool matching_publisher =
      (lane != nullptr && lane->publisher_uuid == state->publisher_uuid) ||
      (malformed != nullptr && malformed->publisher_uuid == state->publisher_uuid);
  if (!matching_publisher) return;

  try {
    const auto parsed = ParseKey(state->prefix, sample.key);
    if (!parsed || parsed->kind != KeyKind::Session || parsed->sid != state->sid) return;
    std::scoped_lock lock(state->sequence_mutex);
    if (lane != nullptr) {
      if (application_started) {
        state->fence_receiver.RecordRejected(lane->sequence, Status::OutcomeUnknown,
                                             lane->sequence);
      } else if (callback_admitted) {
        state->fence_receiver.RecordRejected(lane->sequence, Status::Error, lane->sequence);
      } else {
        state->fence_receiver.RecordOverflow(lane->sequence);
      }
    } else {
      state->fence_receiver.RecordMalformed(malformed->sequence);
    }
    return;
  } catch (...) {
  }

  // If the sequence proof itself cannot be retained, close this receiver
  // registration. A later marker then remains uncompleted instead of claiming
  // a successful prefix over an observation that the callback discarded.
  try {
    if (state->fence_dispatcher) {
      state->fence_dispatcher->CloseAndWait(state->fence_registration);
    }
  } catch (...) {
  }
}

void DispatchSample(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
                    const TransportSample& sample) {
  bool callback_admitted = false;
  bool application_started = false;
  try {
    {
      std::scoped_lock lock(state->fence_test_mutex);
      if (std::exchange(state->fence_test_throw_dispatch_once, false)) {
        throw std::bad_alloc();
      }
    }
    bool marker = false;
    bool participating = false;
    auto route = ParseFenceMarkerRoute(state->prefix, sample.key);
    if (!route.has_value()) {
      route = fence_internal::RecoverMalformedFenceMarkerRoute(state->prefix, sample.key);
    }
    if (route && route->target == FenceMarkerTarget::Cache && route->sid == state->sid &&
        route->receiver_generation == state->attach_generation) {
      marker = true;
      participating = true;
    } else if (const auto parsed = ParseKey(state->prefix, sample.key);
               parsed && parsed->kind == KeyKind::Session && parsed->sid == state->sid) {
      if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
        participating = lane->publisher_uuid == state->publisher_uuid;
      } else if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
        participating = malformed->publisher_uuid == state->publisher_uuid;
      }
    }
    if (!participating || !state->fence_dispatcher) {
      OnSample(state, sample, false);
      return;
    }
    const auto prepare = [&] {
      if (marker) return true;
      std::scoped_lock lock(state->sequence_mutex);
      if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
        return state->fence_receiver.Admit(lane->sequence);
      }
      if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
        state->fence_receiver.RecordMalformed(malformed->sequence);
      }
      return false;
    };
    const auto overflow = [&] {
      if (marker) return;
      std::scoped_lock lock(state->sequence_mutex);
      if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
        state->fence_receiver.RecordOverflow(lane->sequence);
      } else if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
        state->fence_receiver.RecordMalformed(malformed->sequence);
      }
    };
    auto admission =
        state->fence_dispatcher->Dispatch(state->fence_registration, prepare, overflow);
    if (admission.outcome != fence_internal::FenceDispatchCoordinator::Outcome::Admitted) return;
    admission.entry.WaitTurn();
    callback_admitted = true;
    OnSample(state, sample, !marker, &application_started);
  } catch (...) {
    // Transport callbacks are an exception boundary, but identifiable covered
    // data must still leave fail-closed receiver evidence.
    RetainCacheDispatchFailure(state, sample, callback_admitted, application_started);
  }
}

Result<void> DecodeGetReply(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
                            bool snapshot, std::string_view full_key,
                            std::span<const std::byte> payload, const Encoding& encoding,
                            param_cache_detail::Access::Impl::ValueMap& out, bool& invalid) {
  const auto parsed = ParseKey(state->prefix, full_key);
  const bool expected =
      parsed.has_value() && !parsed->is_batch &&
      ((snapshot && parsed->kind == KeyKind::Snapshot && parsed->sid == state->sid) ||
       (!snapshot && parsed->kind == KeyKind::Session && parsed->sid == state->sid));
  if (!expected) {
    invalid = true;
    return Result<void>::Err(Status::Error, "transport returned an invalid cache key");
  }
  if (encoding.id == Encoding::kSitosV1Batch) {
    invalid = true;
    return Result<void>::Err(Status::Error, "transport returned a batch for a value query");
  }
  std::optional<ParamValue> value;
  if (encoding.id == Encoding::kSitosV1) {
    value = ParamValue::Decode(payload);
    if (!value.has_value()) {
      invalid = true;
      return Result<void>::Err(Status::Error, "transport returned malformed payload");
    }
  } else {
    value = ParamValue(std::vector<std::byte>(payload.begin(), payload.end()));
  }
  out[parsed->relative_key] = std::make_shared<const ParamValue>(std::move(*value));
  return Result<void>::Ok();
}

Result<void> Fetch(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
                   const std::shared_ptr<Transport>& transport, std::string_view query,
                   bool snapshot, param_cache_detail::Access::Impl::ValueMap& out,
                   std::chrono::milliseconds timeout) {
  bool invalid = false;
  Result<void> protocol_error = Result<void>::Ok();
  const auto sink = [&state, snapshot, &out, &invalid, &protocol_error](
                        std::string_view key, std::span<const std::byte> payload,
                        Encoding encoding) {
    auto decoded = DecodeGetReply(state, snapshot, key, payload, encoding, out, invalid);
    const bool ok = decoded.IsOk();
    if (!ok) protocol_error = std::move(decoded);
    return ok;
  };
  auto result = transport->Get(query, sink, timeout);
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  if (invalid) return Result<void>::ErrFrom(protocol_error);
  return Result<void>::Ok();
}

void CleanupCandidate(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
                      Subscription& subscription, Subscription& marker_subscription) {
  CloseGate(state);
  if (state->fence_dispatcher) {
    state->fence_dispatcher->CloseAndWait(state->fence_registration);
  }
  marker_subscription = Subscription{};
  subscription = Subscription{};
  WaitForCallbacks(state);
  std::lock_guard sequence_lock(state->sequence_mutex);
  state->phase = param_cache_detail::Access::Impl::Phase::Stopping;
  std::unique_lock map_lock(state->map_mutex);
  state->snapshot_baseline.clear();
  state->effective_map.clear();
  state->buffered.clear();
  state->buffered_fence_observations.clear();
}

}  // namespace

ParamCache::ParamCache(std::shared_ptr<Transport> transport, ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(transport), std::move(config))) {}

ParamCache::~ParamCache() { Detach(); }

ParamCache::ParamCache(ParamCache&&) noexcept = default;

ParamCache& ParamCache::operator=(ParamCache&& other) noexcept {
  if (this == &other) return *this;
  Detach();
  impl_ = std::move(other.impl_);
  return *this;
}

Result<ParamCache> ParamCache::Open(ClientConfig config) {
  auto validation = ValidateClientConfig(config);
  if (!validation.IsOk()) return Result<ParamCache>::ErrFrom(validation);
  std::optional<std::string_view> json;
  if (config.zenoh_config_json.has_value()) json = *config.zenoh_config_json;
  auto transport_result = OpenZenohTransport(json);
  if (!transport_result.IsOk()) return Result<ParamCache>::ErrFrom(transport_result);
  std::shared_ptr<Transport> transport(std::move(transport_result).Value());
  return Result<ParamCache>::Ok(ParamCache(std::move(transport), std::move(config)));
}

Result<ParamCache> ParamCache::Open(std::shared_ptr<Transport> transport, ClientConfig config) {
  if (!transport) return Result<ParamCache>::Err(Status::InvalidArgument, "null transport");
  auto validation = ValidateClientConfig(config);
  if (!validation.IsOk()) return Result<ParamCache>::ErrFrom(validation);
  if (config.zenoh_config_json.has_value()) {
    return Result<ParamCache>::Err(Status::InvalidArgument,
                                   "injected transport cannot apply zenoh configuration");
  }
  return Result<ParamCache>::Ok(ParamCache(std::move(transport), std::move(config)));
}

Result<std::shared_ptr<const ParamValue>> ParamCache::GetShared(std::string_view key) const {
  auto key_result = ValidateCacheKey(key);
  if (!key_result.IsOk()) return Result<std::shared_ptr<const ParamValue>>::ErrFrom(key_result);
  if (!impl_) {
    return Result<std::shared_ptr<const ParamValue>>::Err(Status::InvalidArgument,
                                                          "moved-from ParamCache");
  }
  const auto state = LoadState(*impl_);
  if (!state) {
    return Result<std::shared_ptr<const ParamValue>>::Err(Status::InvalidArgument,
                                                          "ParamCache is detached");
  }
  NotifyReadStateLoaded(*state);
  std::shared_lock lock(state->map_mutex);
  const auto it = state->effective_map.find(key);
  if (it == state->effective_map.end()) {
    return Result<std::shared_ptr<const ParamValue>>::Err(Status::NotFound,
                                                          "parameter key not found");
  }
  return Result<std::shared_ptr<const ParamValue>>::Ok(it->second);
}

Result<bool> ParamCache::Contains(std::string_view key) const {
  auto key_result = ValidateCacheKey(key);
  if (!key_result.IsOk()) return Result<bool>::ErrFrom(key_result);
  if (!impl_) return Result<bool>::Err(Status::InvalidArgument, "moved-from ParamCache");
  const auto state = LoadState(*impl_);
  if (!state) return Result<bool>::Err(Status::InvalidArgument, "ParamCache is detached");
  NotifyReadStateLoaded(*state);
  std::shared_lock lock(state->map_mutex);
  return Result<bool>::Ok(state->effective_map.find(key) != state->effective_map.end());
}

Result<void> ParamCache::List(std::string_view prefix, const ListSink& sink) const {
  if (!sink) return InvalidArgument("null List sink");
  if (!prefix.empty()) {
    if (prefix.front() == '/' || prefix.find("//") != std::string_view::npos ||
        prefix.find_first_of("*?#$%@:") != std::string_view::npos ||
        prefix.find_first_of(" \t\r\n") != std::string_view::npos) {
      return InvalidKey("invalid List prefix");
    }
    const auto chunks = prefix.ends_with('/') ? prefix.substr(0, prefix.size() - 1) : prefix;
    if (!IsValidKey(chunks)) return InvalidKey("invalid List prefix");
  }
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  const auto state = LoadState(*impl_);
  if (!state) return InvalidArgument("ParamCache is detached");
  NotifyReadStateLoaded(*state);
  std::vector<std::pair<std::string, std::shared_ptr<const ParamValue>>> values;
  {
    std::shared_lock lock(state->map_mutex);
    values.reserve(state->effective_map.size());
    for (const auto& [key, value] : state->effective_map) {
      if (MatchesPrefix(key, prefix)) values.emplace_back(key, value);
    }
  }
  std::sort(values.begin(), values.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  for (const auto& [key, value] : values) {
    if (!sink(key, *value)) break;
  }
  return Result<void>::Ok();
}

Result<void> ParamCache::Put(std::string_view key, const ParamValue& value) {
  auto key_result = ValidateCacheKey(key);
  if (!key_result.IsOk()) return key_result;
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  const auto state = LoadState(*impl_);
  if (!state) return InvalidArgument("ParamCache is detached");
  const auto full_key = BuildKey(state->prefix, "session/" + state->sid, key);
  if (!full_key) return InvalidKey("invalid cache key");
  auto payload = value.Encode();
  OperationLease lease(state);
  if (!lease) return InvalidArgument("ParamCache is detached");
  Result<void> result = state->fence_publisher
                            ? state->fence_publisher->SubmitData(
                                  *full_key, payload, Encoding{std::string(Encoding::kSitosV1)})
                            : impl_->transport->Put(*full_key, payload,
                                                    Encoding{std::string(Encoding::kSitosV1)}, {});
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  const auto owned = std::make_shared<const ParamValue>(value);
  const Impl::Mutation mutation{Impl::MutationKind::Put, std::string(key), owned};
  std::lock_guard sequence_lock(state->sequence_mutex);
  if (state->phase != Impl::Phase::Live) return InvalidArgument("ParamCache is detached");
  ApplyMutations(*state, std::vector<Impl::Mutation>{mutation});
  return Result<void>::Ok();
}

Result<void> ParamCache::PutBatch(std::span<const BatchEntry> entries) {
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  const auto state = LoadState(*impl_);
  if (!state) return InvalidArgument("ParamCache is detached");
  for (const auto& entry : entries) {
    if (!IsValidKey(entry.key)) return InvalidKey("invalid batch key");
  }
  if (entries.empty()) return Result<void>::Ok();
  const auto full_key = BuildBatchKey(state->prefix, "session/" + state->sid);
  if (!full_key) return InvalidKey("invalid batch scope");
  auto payload = EncodeBatch(entries);
  OperationLease lease(state);
  if (!lease) return InvalidArgument("ParamCache is detached");
  Result<void> result =
      state->fence_publisher
          ? state->fence_publisher->SubmitData(*full_key, payload,
                                               Encoding{std::string(Encoding::kSitosV1Batch)})
          : impl_->transport->Put(*full_key, payload,
                                  Encoding{std::string(Encoding::kSitosV1Batch)}, {});
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  std::vector<Impl::Mutation> mutations;
  mutations.reserve(entries.size());
  for (const auto& entry : entries) {
    mutations.push_back(Impl::Mutation{Impl::MutationKind::Put, entry.key,
                                       std::make_shared<const ParamValue>(entry.value)});
  }
  std::lock_guard sequence_lock(state->sequence_mutex);
  if (state->phase != Impl::Phase::Live) return InvalidArgument("ParamCache is detached");
  ApplyMutations(*state, mutations);
  return Result<void>::Ok();
}

Result<void> ParamCache::Attach(std::string_view sid) {
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  if (!IsValidSessionId(sid)) return InvalidKey("invalid session id");
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  if (LoadState(*impl_)) return InvalidArgument("ParamCache is already attached");

  auto state = std::make_shared<Impl::State>(impl_->config.prefix, std::string(sid));
  state->fence_dispatcher = impl_->transport->FenceDispatcher();
  if (state->fence_dispatcher) {
    state->fence_registration = state->fence_dispatcher->Register();
  }
  if (impl_->transport->SupportsFenceProfile()) {
    state->fence_publisher = std::make_unique<fence_internal::FencePublisher>(
        *impl_->transport, state->publisher_uuid,
        fence_internal::FencePublisherBinding{fence_internal::FencePublisherTarget::Cache,
                                              state->prefix, state->sid, state->attach_generation,
                                              std::nullopt, AckDurability::Applied});
  }
  Subscription subscription;
  Subscription marker_subscription;
  auto declared = impl_->transport->DeclareSubscriber(
      ScopeQuery(impl_->config, "session/" + std::string(sid)),
      [state](const TransportSample& sample) { DispatchSample(state, sample); });
  if (!declared.IsOk()) {
    CloseGate(state);
    WaitForCallbacks(state);
    return Result<void>::ErrFrom(declared);
  }
  subscription = std::move(declared).Value();
  const auto marker_selector = state->prefix + "/meta/fence/cache/" + state->sid + "/**";
  auto marker_declared = impl_->transport->DeclareSubscriber(
      marker_selector, [state](const TransportSample& sample) { DispatchSample(state, sample); });
  if (!marker_declared.IsOk()) {
    CleanupCandidate(state, subscription, marker_subscription);
    return Result<void>::ErrFrom(marker_declared);
  }
  marker_subscription = std::move(marker_declared).Value();

  param_cache_detail::Access::Impl::ValueMap snapshot;
  param_cache_detail::Access::Impl::ValueMap overlay;
  auto snapshot_result =
      Fetch(state, impl_->transport, ScopeQuery(impl_->config, "snap/" + std::string(sid)), true,
            snapshot, impl_->config.query_timeout);
  if (!snapshot_result.IsOk()) {
    CleanupCandidate(state, subscription, marker_subscription);
    return snapshot_result;
  }
  auto overlay_result =
      Fetch(state, impl_->transport, ScopeQuery(impl_->config, "session/" + std::string(sid)),
            false, overlay, impl_->config.query_timeout);
  if (!overlay_result.IsOk()) {
    CleanupCandidate(state, subscription, marker_subscription);
    return overlay_result;
  }

  {
    std::lock_guard sequence_lock(state->sequence_mutex);
    {
      std::unique_lock map_lock(state->map_mutex);
      state->snapshot_baseline = std::move(snapshot);
      state->effective_map = state->snapshot_baseline;
      for (auto& [key, value] : overlay) state->effective_map[key] = std::move(value);
    }
    ApplyMutations(*state, state->buffered);
    state->callback_mutation_count += state->buffered.size();
    for (const auto& observation : state->buffered_fence_observations) {
      CompleteFenceObservation(*state, observation);
    }
    state->buffered.clear();
    state->buffered_fence_observations.clear();
    state->phase = Impl::Phase::Live;
  }
  StoreState(*impl_, state);
  impl_->subscription = std::move(subscription);
  impl_->marker_subscription = std::move(marker_subscription);
  return Result<void>::Ok();
}

void ParamCache::Detach() noexcept {
  if (!impl_) return;
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  const auto state = LoadState(*impl_);
  if (!state) return;
  if (state->fence_publisher) state->fence_publisher->Close();
  CloseGate(state);
  if (state->fence_dispatcher) {
    state->fence_dispatcher->CloseAndWait(state->fence_registration);
  }
  impl_->marker_subscription = Subscription{};
  impl_->subscription = Subscription{};
  WaitForCallbacks(state);
  std::lock_guard sequence_lock(state->sequence_mutex);
  state->phase = Impl::Phase::Stopping;
  StoreState(*impl_, nullptr);
}

namespace param_cache_test_access {

bool ParamCacheTestAccess::IsAttached(const ParamCache& cache) {
  return cache.impl_ != nullptr && LoadState(*cache.impl_) != nullptr;
}

std::size_t ParamCacheTestAccess::Size(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return 0;
  std::shared_lock lock(state->map_mutex);
  return state->effective_map.size();
}

std::optional<ParamCacheTestAccess::GateState> ParamCacheTestAccess::GetGateState(
    const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return std::nullopt;
  std::lock_guard lock(state->gate_mutex);
  return GateState{state->accepting, state->in_flight};
}

std::optional<ParamValue> ParamCacheTestAccess::Get(const ParamCache& cache, std::string_view key) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return std::nullopt;
  std::shared_lock lock(state->map_mutex);
  const auto it = state->effective_map.find(key);
  if (it == state->effective_map.end()) return std::nullopt;
  return *it->second;
}

void ParamCacheTestAccess::SetCallbackHook(ParamCache& cache, std::function<void()> hook) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return;
  state->callback_hook = std::move(hook);
}

void ParamCacheTestAccess::SetReadStateHook(ParamCache& cache, std::function<void()> hook) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return;
  state->read_state_hook = std::move(hook);
}

void ParamCacheTestAccess::SetMutationHook(ParamCache& cache,
                                           std::function<void(std::size_t)> hook) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return;
  state->mutation_hook = std::move(hook);
}

}  // namespace param_cache_test_access

namespace fence_test_access {

bool FenceTestAccess::ConfigureCacheFenceReceiver(ParamCache& cache,
                                                  const FenceUuid& attach_generation,
                                                  const FenceUuid& publisher_uuid) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return false;
  if (state->fence_publisher) state->fence_publisher->Close();
  std::scoped_lock sequence_lock(state->sequence_mutex);
  state->attach_generation = attach_generation;
  state->publisher_uuid = publisher_uuid;
  state->fence_receiver.Reset();
  state->fence_publisher = std::make_unique<fence_internal::FencePublisher>(
      *cache.impl_->transport, publisher_uuid,
      fence_internal::FencePublisherBinding{fence_internal::FencePublisherTarget::Cache,
                                            state->prefix, state->sid, attach_generation,
                                            std::nullopt, AckDurability::Applied});
  return true;
}

std::uint64_t FenceTestAccess::CacheCompletedThrough(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return 0;
  std::scoped_lock lock(state->sequence_mutex);
  return state->fence_receiver.completed_through();
}

std::size_t FenceTestAccess::CacheMutationCount(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return 0;
  std::scoped_lock lock(state->sequence_mutex);
  return state->callback_mutation_count;
}

Result<fence_internal::FenceHandle> FenceTestAccess::BeginCacheFence(
    ParamCache& cache, std::chrono::milliseconds deadline) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state || !state->fence_publisher) {
    return Result<fence_internal::FenceHandle>::Err(Status::InvalidArgument);
  }
  auto result = state->fence_publisher->BeginFence(deadline);
  if (result.IsOk()) {
    std::scoped_lock lock(state->fence_test_mutex);
    state->last_fence_handle = result.Value();
  }
  return result;
}

Result<fence_internal::FenceHandle> FenceTestAccess::PublishCacheWaiterForTesting(
    ParamCache& cache, std::uint64_t through_sequence, std::chrono::milliseconds deadline) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state || !state->fence_publisher) {
    return Result<fence_internal::FenceHandle>::Err(Status::InvalidArgument);
  }
  auto result = state->fence_publisher->PublishWaiterForTesting(through_sequence, deadline);
  if (result.IsOk()) {
    std::scoped_lock lock(state->fence_test_mutex);
    state->last_fence_handle = result.Value();
  }
  return result;
}

Result<fence_internal::FenceHandle> FenceTestAccess::PublishCacheWaiterWithToken(
    ParamCache& cache, std::uint64_t through_sequence, std::chrono::milliseconds deadline,
    const AckToken& token) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state || !state->fence_publisher) {
    return Result<fence_internal::FenceHandle>::Err(Status::InvalidArgument);
  }
  auto result = state->fence_publisher->PublishWaiterForTesting(through_sequence, deadline, token);
  if (result.IsOk()) {
    std::scoped_lock lock(state->fence_test_mutex);
    state->last_fence_handle = result.Value();
  }
  return result;
}

bool FenceTestAccess::ReceiveCacheMarker(ParamCache& cache, const FenceUuid& attach_generation,
                                         const FenceUuid& publisher_uuid, const AckToken& token) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state || !state->fence_publisher || state->attach_generation != attach_generation ||
      state->publisher_uuid != publisher_uuid) {
    return false;
  }
  const auto through = state->fence_publisher->PendingThrough(token);
  if (!through.has_value()) return false;
  AckResultV1 result;
  {
    std::scoped_lock lock(state->sequence_mutex);
    result = state->fence_receiver.Evaluate(*through);
  }
  return state->fence_publisher->Complete(token, std::move(result));
}

std::optional<AckResultV1> FenceTestAccess::CacheFenceResult(const ParamCache& cache,
                                                             const AckToken& token) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return std::nullopt;
  std::optional<fence_internal::FenceHandle> handle;
  {
    std::scoped_lock lock(state->fence_test_mutex);
    if (!state->last_fence_handle.has_value() || state->last_fence_handle->token != token) {
      return std::nullopt;
    }
    handle = state->last_fence_handle;
  }
  std::scoped_lock lock(handle->waiter->mutex);
  return handle->waiter->result;
}

AckResultV1 FenceTestAccess::CacheFirstFailure(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) {
    return AckResultV1{AckOperationKind::Fence, Status::Disconnected,
                       AckDurability::Applied,  0,
                       kAckNoFailedIndex,       0,
                       kAckNoFailedSequence,    ""};
  }
  std::scoped_lock lock(state->sequence_mutex);
  return state->fence_receiver.Evaluate(UINT64_MAX);
}

bool FenceTestAccess::CacheContains(const ParamCache& cache, std::string_view key) {
  const auto result = cache.Contains(key);
  return result.IsOk() && result.Value();
}

bool FenceTestAccess::CacheFencePending(const ParamCache& cache, const AckToken& token) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  return state && state->fence_publisher && state->fence_publisher->WaiterPublished(token);
}

FenceUuid FenceTestAccess::CacheAttachGeneration(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  return state ? state->attach_generation : FenceUuid{};
}

bool FenceTestAccess::ThrowCacheDispatchOnce(ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_throw_dispatch_once = true;
  return true;
}

bool FenceTestAccess::GateCacheCallback(ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return false;
  const std::weak_ptr weak_state(state);
  state->callback_hook = [weak_state] {
    const auto active = weak_state.lock();
    if (!active) return;
    std::unique_lock lock(active->fence_test_mutex);
    active->fence_test_callback_blocked = true;
    active->fence_test_condition.notify_all();
    active->fence_test_condition.wait(lock,
                                      [&active] { return active->fence_test_callback_released; });
  };
  return true;
}

bool FenceTestAccess::WaitForCacheCallbackBlocked(ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return false;
  std::unique_lock lock(state->fence_test_mutex);
  state->fence_test_condition.wait(lock, [state] { return state->fence_test_callback_blocked; });
  return true;
}

bool FenceTestAccess::WaitUntilCacheAdmissionClosed(ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return true;
  std::unique_lock lock(state->gate_mutex);
  state->gate_condition.wait(lock, [state] { return !state->accepting; });
  return true;
}

bool FenceTestAccess::LateCacheMarkerCanAccessState(ParamCache& cache,
                                                    const FenceUuid& attach_generation,
                                                    const FenceUuid& publisher_uuid,
                                                    const AckToken& token) {
  return ReceiveCacheMarker(cache, attach_generation, publisher_uuid, token);
}

bool FenceTestAccess::ReleaseCacheCallback(ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_callback_released = true;
  state->fence_test_condition.notify_all();
  return true;
}

bool FenceTestAccess::CacheCallbacksQuiesced(const ParamCache& cache) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return true;
  std::scoped_lock lock(state->gate_mutex);
  return state->in_flight == 0;
}

}  // namespace fence_test_access

}  // namespace sitos
