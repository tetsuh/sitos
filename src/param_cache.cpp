// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/key.hpp"
#include "param_cache_test_access.hpp"

namespace sitos {
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

}  // namespace

struct ParamCache::Impl {
  enum class Phase { Buffering, Live, Stopping };
  enum class MutationKind { Put, Delete };

  struct Mutation {
    MutationKind kind;
    std::string key;
    std::shared_ptr<const ParamValue> value;
  };

  struct State {
    explicit State(std::string prefix_value, bool session_value, std::string sid_value)
        : prefix(std::move(prefix_value)), session(session_value), sid(std::move(sid_value)) {}

    std::string prefix;
    bool session;
    std::string sid;
    Phase phase = Phase::Buffering;
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool accepting = true;
    std::size_t in_flight = 0;
    std::mutex sequence_mutex;
    mutable std::shared_mutex map_mutex;
    std::unordered_map<std::string, std::shared_ptr<const ParamValue>> snapshot_baseline;
    std::unordered_map<std::string, std::shared_ptr<const ParamValue>> effective_map;
    std::vector<Mutation> buffered;
  };

  explicit Impl(std::shared_ptr<Transport> transport_value, ClientConfig config_value)
      : transport(std::move(transport_value)), config(std::move(config_value)) {}

  std::shared_ptr<Transport> transport;
  ClientConfig config;
  std::mutex lifecycle_mutex;
  std::shared_ptr<State> active_state;
  Subscription subscription;
};

namespace {

class CallbackLease {
 public:
  explicit CallbackLease(const std::shared_ptr<ParamCache::Impl::State>& state) : state_(state) {
    std::lock_guard lock(state_->gate_mutex);
    if (!state_->accepting) return;
    ++state_->in_flight;
    entered_ = true;
  }

  ~CallbackLease() {
    if (!entered_) return;
    std::lock_guard lock(state_->gate_mutex);
    --state_->in_flight;
    if (state_->in_flight == 0) state_->gate_condition.notify_all();
  }

  explicit operator bool() const noexcept { return entered_; }

 private:
  std::shared_ptr<ParamCache::Impl::State> state_;
  bool entered_ = false;
};

void CloseGate(const std::shared_ptr<ParamCache::Impl::State>& state) {
  std::lock_guard lock(state->gate_mutex);
  state->accepting = false;
}

void WaitForCallbacks(const std::shared_ptr<ParamCache::Impl::State>& state) {
  std::unique_lock lock(state->gate_mutex);
  state->gate_condition.wait(lock, [&] { return state->in_flight == 0; });
}

bool IsExpected(const ParsedKey& parsed, const ParamCache::Impl::State& state,
                bool batch_allowed) {
  if (parsed.is_batch != batch_allowed) return false;
  if (state.session) {
    return parsed.kind == KeyKind::Session && parsed.sid == state.sid;
  }
  return parsed.kind == KeyKind::Base;
}

std::optional<ParamCache::Impl::Mutation> DecodeOrdinary(
    const ParamCache::Impl::State& state, const TransportSample& sample) {
  const auto parsed = ParseKey(state.prefix, sample.key);
  if (!parsed || !IsExpected(*parsed, state, false)) return std::nullopt;
  if (sample.kind == TransportSample::Kind::Delete) {
    return ParamCache::Impl::Mutation{ParamCache::Impl::MutationKind::Delete,
                                      parsed->relative_key, nullptr};
  }
  if (sample.encoding.id == Encoding::kSitosV1Batch) return std::nullopt;

  std::optional<ParamValue> decoded;
  if (sample.encoding.id == Encoding::kSitosV1) {
    decoded = ParamValue::Decode(sample.payload);
    if (!decoded.has_value()) return std::nullopt;
  } else {
    decoded = ParamValue(std::vector<std::byte>(sample.payload.begin(), sample.payload.end()));
  }
  return ParamCache::Impl::Mutation{ParamCache::Impl::MutationKind::Put,
                                    parsed->relative_key,
                                    std::make_shared<const ParamValue>(std::move(*decoded))};
}

std::optional<std::vector<ParamCache::Impl::Mutation>> DecodeSample(
    const std::shared_ptr<ParamCache::Impl::State>& state, const TransportSample& sample) {
  if (sample.kind == TransportSample::Kind::Delete) {
    auto mutation = DecodeOrdinary(*state, sample);
    if (!mutation.has_value()) return std::nullopt;
    return std::vector<ParamCache::Impl::Mutation>{std::move(*mutation)};
  }

  const auto parsed = ParseKey(state->prefix, sample.key);
  if (!parsed) return std::nullopt;
  if (parsed->is_batch) {
    if (!IsExpected(*parsed, *state, true) || sample.encoding.id != Encoding::kSitosV1Batch) {
      return std::nullopt;
    }
    auto entries = DecodeBatch(sample.payload);
    if (!entries.has_value()) return std::nullopt;
    std::vector<ParamCache::Impl::Mutation> mutations;
    mutations.reserve(entries->size());
    for (auto& entry : *entries) {
      if (!IsValidKey(entry.key)) return std::nullopt;
      mutations.push_back(ParamCache::Impl::Mutation{
          ParamCache::Impl::MutationKind::Put, std::move(entry.key),
          std::make_shared<const ParamValue>(std::move(entry.value))});
    }
    return mutations;
  }
  auto ordinary = DecodeOrdinary(*state, sample);
  if (!ordinary.has_value()) return std::nullopt;
  return std::vector<ParamCache::Impl::Mutation>{std::move(*ordinary)};
}

void ApplyMutation(ParamCache::Impl::State& state,
                   const ParamCache::Impl::Mutation& mutation) {
  std::unique_lock lock(state.map_mutex);
  if (mutation.kind == ParamCache::Impl::MutationKind::Put) {
    state.effective_map[mutation.key] = mutation.value;
    return;
  }
  if (state.session) {
    const auto baseline = state.snapshot_baseline.find(mutation.key);
    if (baseline != state.snapshot_baseline.end()) {
      state.effective_map[mutation.key] = baseline->second;
    } else {
      state.effective_map.erase(mutation.key);
    }
  } else {
    state.effective_map.erase(mutation.key);
  }
}

void ApplyMutations(ParamCache::Impl::State& state,
                    const std::vector<ParamCache::Impl::Mutation>& mutations) {
  for (const auto& mutation : mutations) ApplyMutation(state, mutation);
}

void OnSample(const std::shared_ptr<ParamCache::Impl::State>& state,
              const TransportSample& sample) {
  CallbackLease lease(state);
  if (!lease) return;
  auto mutations = DecodeSample(state, sample);
  if (!mutations.has_value()) return;

  std::lock_guard sequence_lock(state->sequence_mutex);
  if (state->phase == ParamCache::Impl::Phase::Buffering) {
    for (auto& mutation : *mutations) state->buffered.push_back(std::move(mutation));
    return;
  }
  if (state->phase != ParamCache::Impl::Phase::Live) return;
  ApplyMutations(*state, *mutations);
}

Result<void> DecodeGetReply(const std::shared_ptr<ParamCache::Impl::State>& state,
                            bool snapshot, std::string_view full_key,
                            std::span<const std::byte> payload, const Encoding& encoding,
                            std::unordered_map<std::string, std::shared_ptr<const ParamValue>>& out,
                            bool& invalid) {
  const auto parsed = ParseKey(state->prefix, full_key);
  const bool expected = parsed.has_value() && !parsed->is_batch &&
                       ((snapshot && parsed->kind == KeyKind::Snapshot &&
                         parsed->sid == state->sid) ||
                        (!snapshot && ((state->session && parsed->kind == KeyKind::Session &&
                                        parsed->sid == state->sid) ||
                                       (!state->session && parsed->kind == KeyKind::Base))));
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

Result<void> Fetch(const std::shared_ptr<ParamCache::Impl::State>& state,
                   const std::shared_ptr<Transport>& transport, std::string_view query,
                   bool snapshot,
                   std::unordered_map<std::string, std::shared_ptr<const ParamValue>>& out,
                   std::chrono::milliseconds timeout) {
  bool invalid = false;
  Result<void> protocol_error = Result<void>::Ok();
  const auto sink = [&](std::string_view key, std::span<const std::byte> payload,
                        Encoding encoding) {
    auto decoded = DecodeGetReply(state, snapshot, key, payload, encoding, out, invalid);
    if (!decoded.IsOk()) protocol_error = std::move(decoded);
    return decoded.IsOk();
  };
  auto result = transport->Get(query, sink, timeout);
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  if (invalid) return Result<void>::ErrFrom(protocol_error);
  return Result<void>::Ok();
}

void CleanupCandidate(const std::shared_ptr<ParamCache::Impl::State>& state,
                      Subscription& subscription) {
  CloseGate(state);
  subscription = Subscription{};
  WaitForCallbacks(state);
  std::lock_guard sequence_lock(state->sequence_mutex);
  state->phase = ParamCache::Impl::Phase::Stopping;
  std::unique_lock map_lock(state->map_mutex);
  state->snapshot_baseline.clear();
  state->effective_map.clear();
  state->buffered.clear();
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

Result<void> ParamCache::Attach(std::string_view sid) {
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  if (!IsValidSessionId(sid)) return InvalidKey("invalid session id");
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  if (impl_->active_state) return InvalidArgument("ParamCache is already attached");

  auto state = std::make_shared<Impl::State>(impl_->config.prefix, true, std::string(sid));
  Subscription subscription;
  auto declared = impl_->transport->DeclareSubscriber(
      ScopeQuery(impl_->config, "session/" + std::string(sid)),
      [state](const TransportSample& sample) { OnSample(state, sample); });
  if (!declared.IsOk()) {
    CloseGate(state);
    WaitForCallbacks(state);
    return Result<void>::ErrFrom(declared);
  }
  subscription = std::move(declared).Value();

  std::unordered_map<std::string, std::shared_ptr<const ParamValue>> snapshot;
  std::unordered_map<std::string, std::shared_ptr<const ParamValue>> overlay;
  auto snapshot_result = Fetch(state, impl_->transport,
                               ScopeQuery(impl_->config, "snap/" + std::string(sid)), true,
                               snapshot, impl_->config.query_timeout);
  if (!snapshot_result.IsOk()) {
    CleanupCandidate(state, subscription);
    return snapshot_result;
  }
  auto overlay_result = Fetch(state, impl_->transport,
                              ScopeQuery(impl_->config, "session/" + std::string(sid)), false,
                              overlay, impl_->config.query_timeout);
  if (!overlay_result.IsOk()) {
    CleanupCandidate(state, subscription);
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
    state->buffered.clear();
    state->phase = Impl::Phase::Live;
  }
  impl_->active_state = state;
  impl_->subscription = std::move(subscription);
  return Result<void>::Ok();
}

Result<void> ParamCache::AttachBase() {
  if (!impl_) return InvalidArgument("moved-from ParamCache");
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  if (impl_->active_state) return InvalidArgument("ParamCache is already attached");

  auto state = std::make_shared<Impl::State>(impl_->config.prefix, false, "");
  Subscription subscription;
  auto declared = impl_->transport->DeclareSubscriber(
      ScopeQuery(impl_->config, "base"),
      [state](const TransportSample& sample) { OnSample(state, sample); });
  if (!declared.IsOk()) {
    CloseGate(state);
    WaitForCallbacks(state);
    return Result<void>::ErrFrom(declared);
  }
  subscription = std::move(declared).Value();
  std::unordered_map<std::string, std::shared_ptr<const ParamValue>> initial;
  auto initial_result = Fetch(state, impl_->transport, ScopeQuery(impl_->config, "base"), false,
                              initial, impl_->config.query_timeout);
  if (!initial_result.IsOk()) {
    CleanupCandidate(state, subscription);
    return initial_result;
  }
  {
    std::lock_guard sequence_lock(state->sequence_mutex);
    {
      std::unique_lock map_lock(state->map_mutex);
      state->effective_map = std::move(initial);
    }
    ApplyMutations(*state, state->buffered);
    state->buffered.clear();
    state->phase = Impl::Phase::Live;
  }
  impl_->active_state = state;
  impl_->subscription = std::move(subscription);
  return Result<void>::Ok();
}

void ParamCache::Detach() noexcept {
  if (!impl_) return;
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  auto state = std::move(impl_->active_state);
  if (!state) return;
  CloseGate(state);
  impl_->subscription = Subscription{};
  WaitForCallbacks(state);
  std::lock_guard sequence_lock(state->sequence_mutex);
  state->phase = Impl::Phase::Stopping;
  std::unique_lock map_lock(state->map_mutex);
  state->snapshot_baseline.clear();
  state->effective_map.clear();
  state->buffered.clear();
}

namespace param_cache_test_access {

bool ParamCacheTestAccess::IsAttached(const ParamCache& cache) {
  return cache.impl_ != nullptr && cache.impl_->active_state != nullptr;
}

std::size_t ParamCacheTestAccess::Size(const ParamCache& cache) {
  if (cache.impl_ == nullptr || cache.impl_->active_state == nullptr) return 0;
  std::shared_lock lock(cache.impl_->active_state->map_mutex);
  return cache.impl_->active_state->effective_map.size();
}

std::optional<ParamValue> ParamCacheTestAccess::Get(const ParamCache& cache,
                                                     std::string_view key) {
  if (cache.impl_ == nullptr || cache.impl_->active_state == nullptr) return std::nullopt;
  std::shared_lock lock(cache.impl_->active_state->map_mutex);
  const auto it = cache.impl_->active_state->effective_map.find(std::string(key));
  if (it == cache.impl_->active_state->effective_map.end()) return std::nullopt;
  return *it->second;
}

std::vector<std::string> ParamCacheTestAccess::Events(const ParamCache&) { return {}; }

}  // namespace param_cache_test_access

}  // namespace sitos
