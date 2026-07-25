// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include "list_prefix_validation.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace sitos {

struct ParamStore::DeclarationControl {
  std::mutex mutex;
};

namespace {

struct OwnedSample {
  std::string key;
  std::vector<std::byte> payload;
  Encoding encoding;
  TransportSample::Kind kind;
};

struct WorkItem {
  explicit WorkItem(OwnedSample value) : sample(std::move(value)) {}

  OwnedSample sample;
  std::mutex mutex;
  std::condition_variable condition;
  bool complete = false;
};

struct SubscriptionState : std::enable_shared_from_this<SubscriptionState> {
  SubscriptionState(std::shared_ptr<Transport> transport_value,
                    std::shared_ptr<ParamStore::DeclarationControl> control_value,
                    std::string prefix_value, Scope scope_value, std::string list_prefix_value,
                    ParamCallback callback_value, std::shared_ptr<LogSink> log_sink_value)
      : transport(std::move(transport_value)),
        control(std::move(control_value)),
        prefix(std::move(prefix_value)),
        scope(std::move(scope_value)),
        list_prefix(std::move(list_prefix_value)),
        callback(std::move(callback_value)),
        log_sink(std::move(log_sink_value)) {}

  std::shared_ptr<Transport> transport;
  std::shared_ptr<ParamStore::DeclarationControl> control;
  std::string prefix;
  Scope scope;
  std::string list_prefix;
  ParamCallback callback;
  std::shared_ptr<LogSink> log_sink;
  Subscription native;

  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<WorkItem>> queue;
  bool accepting = true;
  bool staging = true;
  bool close_started = false;
  bool close_finished = false;
  bool drainer = false;
  std::thread::id drainer_id;
  std::size_t native_in_flight = 0;
};

std::string ScopePath(const Scope& scope) {
  if (scope.kind == ScopeKind::Base) return "base";
  if (scope.kind == ScopeKind::Session) return "session/" + scope.sid;
  return "snap/" + scope.sid;
}

bool MatchesScope(const ParsedKey& parsed, const Scope& scope) {
  if (scope.kind == ScopeKind::Base) return parsed.kind == KeyKind::Base;
  return parsed.kind == KeyKind::Session && parsed.sid == scope.sid;
}

void Warn(const std::shared_ptr<SubscriptionState>& state, std::string_view message) {
  EmitLog(state->log_sink, LogLevel::kWarning, "ParamSubscription", message);
}

void Error(const std::shared_ptr<SubscriptionState>& state, std::string_view message) {
  EmitLog(state->log_sink, LogLevel::kError, "ParamSubscription", message);
}

std::optional<std::vector<ParamChange>> Decode(
    const std::shared_ptr<SubscriptionState>& state, const OwnedSample& sample) {
  auto parsed = ParseKey(state->prefix, sample.key);
  if (!parsed || !MatchesScope(*parsed, state->scope)) return std::nullopt;

  if (sample.kind == TransportSample::Kind::Delete) {
    if (parsed->is_batch) {
      Warn(state, "batch DELETE is unsupported");
      return std::vector<ParamChange>{};
    }
    if (!state->list_prefix.empty() && !parsed->relative_key.starts_with(state->list_prefix)) {
      return std::vector<ParamChange>{};
    }
    return std::vector<ParamChange>{
        ParamChange{ParamChangeKind::kDelete, std::move(parsed->relative_key), std::nullopt}};
  }

  if (parsed->is_batch) {
    if (sample.encoding.id != Encoding::kSitosV1Batch) {
      Warn(state, "batch sample has an unsupported encoding");
      return std::vector<ParamChange>{};
    }
    auto entries = DecodeBatch(sample.payload);
    if (!entries.has_value()) {
      Warn(state, "malformed batch sample");
      return std::vector<ParamChange>{};
    }
    std::vector<ParamChange> changes;
    changes.reserve(entries->size());
    for (auto& entry : *entries) {
      if (!IsValidKey(entry.key)) {
        Warn(state, "batch sample contains an invalid key");
        return std::vector<ParamChange>{};
      }
      if (state->list_prefix.empty() || entry.key.starts_with(state->list_prefix)) {
        changes.push_back(ParamChange{ParamChangeKind::kPut, std::move(entry.key),
                                      std::move(entry.value)});
      }
    }
    return changes;
  }

  if (!state->list_prefix.empty() && !parsed->relative_key.starts_with(state->list_prefix)) {
    return std::vector<ParamChange>{};
  }
  if (sample.encoding.id == Encoding::kSitosV1Batch) {
    Warn(state, "ordinary sample has batch encoding");
    return std::vector<ParamChange>{};
  }

  std::vector<std::byte> raw(sample.payload.begin(), sample.payload.end());
  ParamValue value = ParamValue(std::move(raw));
  if (sample.encoding.id == Encoding::kSitosV1) {
    auto decoded = ParamValue::Decode(sample.payload);
    if (!decoded.has_value()) {
      Warn(state, "malformed sitos.v1 sample");
      return std::vector<ParamChange>{};
    }
    value = std::move(*decoded);
  } else {
    Warn(state, "unknown sample encoding; wrapped as bytes");
  }
  return std::vector<ParamChange>{
      ParamChange{ParamChangeKind::kPut, std::move(parsed->relative_key), std::move(value)}};
}

void Deliver(const std::shared_ptr<SubscriptionState>& state,
             const std::shared_ptr<WorkItem>& item) {
  auto changes = Decode(state, item->sample);
  if (changes.has_value()) {
    for (const auto& change : *changes) {
      try {
        state->callback(change);
      } catch (const std::exception& exception) {
        Error(state, exception.what());
      } catch (...) {
        Error(state, "ParamSubscription callback threw a non-standard exception");
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(item->mutex);
    item->complete = true;
  }
  item->condition.notify_all();
}

void Drain(const std::shared_ptr<SubscriptionState>& state) {
  for (;;) {
    std::shared_ptr<WorkItem> item;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->queue.empty()) {
        state->drainer = false;
        state->drainer_id = {};
        state->condition.notify_all();
        return;
      }
      item = std::move(state->queue.front());
      state->queue.pop_front();
    }
    Deliver(state, item);
  }
}

void CompleteNative(const std::shared_ptr<SubscriptionState>& state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  --state->native_in_flight;
  state->condition.notify_all();
}

void OnSample(const std::shared_ptr<SubscriptionState>& state, const TransportSample& sample) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting) return;
    ++state->native_in_flight;
  }
  struct NativeGuard {
    std::shared_ptr<SubscriptionState> state;
    ~NativeGuard() { CompleteNative(state); }
  } guard{state};

  std::vector<std::byte> payload(sample.payload.begin(), sample.payload.end());
  OwnedSample owned{sample.key, std::move(payload), sample.encoding, sample.kind};
  auto item = std::make_shared<WorkItem>(std::move(owned));
  bool become_drainer = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting) return;
    state->queue.push_back(item);
    if (state->staging) return;
    if (!state->drainer) {
      state->drainer = true;
      state->drainer_id = std::this_thread::get_id();
      become_drainer = true;
    } else if (state->drainer_id == std::this_thread::get_id()) {
      return;
    }
  }
  if (become_drainer) {
    Drain(state);
    return;
  }
  std::unique_lock<std::mutex> item_lock(item->mutex);
  item->condition.wait(item_lock, [&] { return item->complete; });
}

void ActivateAndDrain(const std::shared_ptr<SubscriptionState>& state) {
  bool become_drainer = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->staging = false;
    if (!state->queue.empty()) {
      state->drainer = true;
      state->drainer_id = std::this_thread::get_id();
      become_drainer = true;
    }
    state->condition.notify_all();
  }
  if (become_drainer) Drain(state);
}

void FailStaging(const std::shared_ptr<SubscriptionState>& state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  state->accepting = false;
  state->staging = false;
  state->queue.clear();
  state->condition.notify_all();
}

void CloseState(const std::shared_ptr<SubscriptionState>& state) noexcept {
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->close_finished) return;
    if (state->close_started) {
      state->condition.wait(lock, [&] { return state->close_finished; });
      return;
    }
    state->close_started = true;
    state->accepting = false;
    state->staging = false;
  }

  {
    std::lock_guard<std::mutex> declaration_lock(state->control->mutex);
    state->native = Subscription{};
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait(lock, [&] {
    return state->native_in_flight == 0 && state->queue.empty() && !state->drainer;
  });
  state->close_finished = true;
  lock.unlock();
  state->condition.notify_all();
}

}  // namespace

struct ParamSubscription::Impl {
  explicit Impl(std::shared_ptr<SubscriptionState> state_value) : state(std::move(state_value)) {}
  std::shared_ptr<SubscriptionState> state;
};

ParamSubscription::ParamSubscription(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ParamSubscription::ParamSubscription(ParamSubscription&& other) noexcept = default;

ParamSubscription& ParamSubscription::operator=(ParamSubscription&& other) noexcept {
  if (this == &other) return *this;
  Close();
  impl_ = std::move(other.impl_);
  return *this;
}

ParamSubscription::~ParamSubscription() { Close(); }

void ParamSubscription::Close() noexcept {
  if (impl_) CloseState(impl_->state);
}

ParamStore::ParamStore(std::shared_ptr<Transport> transport, ClientConfig config)
    : declaration_control_(std::make_shared<DeclarationControl>()),
      transport_(std::move(transport)),
      config_(std::move(config)) {}

ParamStore::ParamStore(ParamStore&& other) noexcept
    : declaration_control_(std::move(other.declaration_control_)),
      transport_(std::move(other.transport_)),
      config_(std::move(other.config_)) {}

ParamStore& ParamStore::operator=(ParamStore&& other) noexcept {
  if (this == &other) return *this;
  declaration_control_ = std::move(other.declaration_control_);
  transport_ = std::move(other.transport_);
  config_ = std::move(other.config_);
  return *this;
}

Result<ParamSubscription> ParamStore::Subscribe(std::string_view scope, std::string_view prefix,
                                                ParamCallback callback) {
  if (!transport_) return Result<ParamSubscription>::Err(Status::InvalidArgument,
                                                         "moved-from ParamStore");
  if (!callback) return Result<ParamSubscription>::Err(Status::InvalidArgument,
                                                       "empty subscription callback");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<ParamSubscription>::ErrFrom(parsed_scope);
  if (parsed_scope.Value().kind == ScopeKind::Snap) {
    return Result<ParamSubscription>::Err(Status::InvalidArgument, "snapshot scope is read-only");
  }
  auto prefix_result = ValidateListPrefix(prefix);
  if (!prefix_result.IsOk()) return Result<ParamSubscription>::ErrFrom(prefix_result);

  const std::string selector = config_.prefix + "/" + ScopePath(parsed_scope.Value()) + "/**";
  auto state = std::make_shared<SubscriptionState>(
      transport_, declaration_control_, config_.prefix, parsed_scope.Value(), std::string(prefix),
      std::move(callback), config_.log_sink);

  std::optional<Result<Subscription>> declared;
  {
    std::lock_guard<std::mutex> declaration_lock(declaration_control_->mutex);
    std::weak_ptr<SubscriptionState> weak_state = state;
    declared.emplace(transport_->DeclareSubscriber(
        selector, [weak_state](const TransportSample& sample) {
          if (auto locked = weak_state.lock()) OnSample(locked, sample);
        }));
    if (declared->IsOk()) state->native = std::move(*declared).Value();
  }
  if (!declared->IsOk()) {
    FailStaging(state);
    return Result<ParamSubscription>::ErrFrom(*declared);
  }

  ActivateAndDrain(state);
  auto impl = std::make_unique<ParamSubscription::Impl>(std::move(state));
  return Result<ParamSubscription>::Ok(ParamSubscription(std::move(impl)));
}

}  // namespace sitos
