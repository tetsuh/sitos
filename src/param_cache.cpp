// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
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
  bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
  bool operator()(const std::string& left, const std::string& right) const noexcept {
    return left == right;
  }
  bool operator()(const std::string& left, std::string_view right) const noexcept { return left == right; }
  bool operator()(std::string_view left, const std::string& right) const noexcept { return left == right; }
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
    std::function<void()> callback_hook;
    std::function<void(std::size_t)> mutation_hook;
    std::size_t mutation_count = 0;
  };

  explicit Impl(std::shared_ptr<Transport> transport_value, ClientConfig config_value)
      : transport(std::move(transport_value)), config(std::move(config_value)) {}

  std::shared_ptr<Transport> transport;
  ClientConfig config;
  std::mutex lifecycle_mutex;
  std::atomic<std::shared_ptr<State>> active_state;
  Subscription subscription;
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
  if (prefix.empty()) return true;
  if (prefix.back() == '/') return key.starts_with(prefix);
  return key.starts_with(prefix);
}

void CloseGate(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state) {
  std::lock_guard lock(state->gate_mutex);
  state->accepting = false;
}

void WaitForCallbacks(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state) {
  std::unique_lock lock(state->gate_mutex);
  state->gate_condition.wait(lock, [state] { return state->in_flight == 0; });
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
    return param_cache_detail::Access::Impl::Mutation{param_cache_detail::Access::Impl::MutationKind::Delete,
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
  return param_cache_detail::Access::Impl::Mutation{param_cache_detail::Access::Impl::MutationKind::Put,
                                    parsed->relative_key,
                                    std::make_shared<const ParamValue>(std::move(*decoded))};
}

std::optional<std::vector<param_cache_detail::Access::Impl::Mutation>> DecodeSample(
    const std::shared_ptr<param_cache_detail::Access::Impl::State>& state, const TransportSample& sample) {
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
                    const std::vector<param_cache_detail::Access::Impl::Mutation>& mutations) {
  for (const auto& mutation : mutations) {
    ApplyMutation(state, mutation);
    ++state.mutation_count;
    if (state.mutation_hook) state.mutation_hook(state.mutation_count);
  }
}

void OnSample(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
              const TransportSample& sample) {
  CallbackLease lease(state);
  if (!lease) return;
  if (state->callback_hook) state->callback_hook();
  auto mutations = DecodeSample(state, sample);
  if (!mutations.has_value()) return;

  std::lock_guard sequence_lock(state->sequence_mutex);
  if (state->phase == param_cache_detail::Access::Impl::Phase::Buffering) {
    for (auto& mutation : *mutations) state->buffered.push_back(std::move(mutation));
    return;
  }
  if (state->phase != param_cache_detail::Access::Impl::Phase::Live) return;
  ApplyMutations(*state, *mutations);
}

Result<void> DecodeGetReply(const std::shared_ptr<param_cache_detail::Access::Impl::State>& state,
                            bool snapshot, std::string_view full_key,
                            std::span<const std::byte> payload, const Encoding& encoding,
                            param_cache_detail::Access::Impl::ValueMap& out,
                            bool& invalid) {
  const auto parsed = ParseKey(state->prefix, full_key);
  const bool expected = parsed.has_value() && !parsed->is_batch &&
                       ((snapshot && parsed->kind == KeyKind::Snapshot &&
                         parsed->sid == state->sid) ||
                        (!snapshot && parsed->kind == KeyKind::Session &&
                         parsed->sid == state->sid));
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
                   bool snapshot,
                   param_cache_detail::Access::Impl::ValueMap& out,
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
                      Subscription& subscription) {
  CloseGate(state);
  subscription = Subscription{};
  WaitForCallbacks(state);
  std::lock_guard sequence_lock(state->sequence_mutex);
  state->phase = param_cache_detail::Access::Impl::Phase::Stopping;
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
  auto result = impl_->transport->Put(*full_key, payload, Encoding{std::string(Encoding::kSitosV1)}, {});
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
  auto result = impl_->transport->Put(*full_key, payload,
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

  param_cache_detail::Access::Impl::ValueMap snapshot;
  param_cache_detail::Access::Impl::ValueMap overlay;
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
  StoreState(*impl_, state);
  impl_->subscription = std::move(subscription);
  return Result<void>::Ok();
}

void ParamCache::Detach() noexcept {
  if (!impl_) return;
  std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
  const auto state = LoadState(*impl_);
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

std::optional<ParamValue> ParamCacheTestAccess::Get(const ParamCache& cache,
                                                     std::string_view key) {
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

void ParamCacheTestAccess::SetMutationHook(
    ParamCache& cache, std::function<void(std::size_t)> hook) {
  const auto state = cache.impl_ == nullptr ? nullptr : LoadState(*cache.impl_);
  if (!state) return;
  state->mutation_hook = std::move(hook);
}

}  // namespace param_cache_test_access

}  // namespace sitos
