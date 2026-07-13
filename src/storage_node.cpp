// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for the base storage scope.

#include "sitos/storage_node.hpp"

#include <utility>
#include <vector>

#include "sitos/key.hpp"
#include "sitos/param_value.hpp"

namespace sitos {
namespace {

std::error_code InvalidArgument() {
  return std::make_error_code(std::errc::invalid_argument);
}

std::error_code OperationInProgress() {
  return std::make_error_code(std::errc::operation_in_progress);
}

std::optional<std::string_view> StripPrefix(std::string_view prefix,
                                            std::string_view keyexpr) {
  if (keyexpr.size() <= prefix.size() || !keyexpr.starts_with(prefix) ||
      keyexpr[prefix.size()] != '/') {
    return std::nullopt;
  }
  return keyexpr.substr(prefix.size() + 1);
}

std::string MakeReplyKey(std::string_view prefix, std::string_view relative_key) {
  std::string key;
  key.reserve(prefix.size() + 6 + relative_key.size());
  key.append(prefix);
  key.append("/base/");
  key.append(relative_key);
  return key;
}

Encoding SitosEncoding() { return Encoding{std::string(Encoding::kSitosV1)}; }

constexpr std::string_view kNodeComponent = "node";
constexpr std::string_view kUnsupportedSubscriberKey = "unsupported subscriber key";
constexpr std::string_view kUnknownSubscriberEncoding =
    "unknown subscriber encoding; wrapped as bytes";
constexpr std::string_view kSubscriberPutFailed = "subscriber PUT failed";
constexpr std::string_view kSubscriberDeleteFailed = "subscriber DELETE failed";
constexpr std::string_view kSubscriberCallbackFailed = "subscriber callback exception";

}  // namespace

std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix,
                                               std::string_view keyexpr) {
  if (!IsValidPrefix(prefix)) return std::nullopt;

  auto rest = StripPrefix(prefix, keyexpr);
  if (!rest || !rest->starts_with("base/")) return std::nullopt;
  std::string_view relative = rest->substr(5);
  if (relative.empty()) return std::nullopt;

  if (relative == "**") return StorageQuery{true, {}};

  if (constexpr std::string_view kSelectorSuffix = "/**";
      relative.ends_with(kSelectorSuffix)) {
    std::string_view selector = relative.substr(0, relative.size() - kSelectorSuffix.size());
    if (!IsValidPrefix(selector)) return std::nullopt;
    std::string list_prefix(selector);
    list_prefix.push_back('/');
    return StorageQuery{true, std::move(list_prefix)};
  }

  if (relative.find('*') != std::string_view::npos || !IsValidKey(relative)) {
    return std::nullopt;
  }
  return StorageQuery{false, std::string(relative)};
}

StorageNode::~StorageNode() { Stop(); }

Result<void> StorageNode::Start(std::shared_ptr<StorageEngine> engine, Config config) {
  Transport* transport = nullptr;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    transport = transport_;
  }
  if (transport == nullptr) return Result<void>::Err(InvalidArgument());
  return Start(std::move(engine), *transport, std::move(config));
}

Result<void> StorageNode::Start(std::shared_ptr<StorageEngine> engine, Transport& transport,
                                Config config) {
  // Serialize declaration and commit, but never hold lifecycle_mutex_ while
  // calling transport code: a fake transport may invoke a staging callback.
  std::unique_lock operation_lock(operation_mutex_);
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (state_ != nullptr) return Result<void>::Err(OperationInProgress());
  }
  if (!engine || !IsValidPrefix(config.prefix)) {
    return Result<void>::Err(InvalidArgument());
  }

  auto state = std::make_shared<State>(std::move(engine), std::move(config.prefix),
                                       std::move(config.log_sink));
  const std::string declaration_key = state->prefix + "/**";
  auto queryable_result = transport.DeclareQueryable(
      declaration_key, [state](TransportQuery& query) { OnQuery(state, query); });
  if (!queryable_result.IsOk()) return Result<void>::Err(queryable_result.Error());
  Queryable queryable = std::move(queryable_result).Value();

  auto subscriber_result = transport.DeclareSubscriber(
      declaration_key, [state](const TransportSample& sample) { OnSample(state, sample); });
  if (!subscriber_result.IsOk()) return Result<void>::Err(subscriber_result.Error());
  Subscription subscriber = std::move(subscriber_result).Value();

  {
    std::scoped_lock lock(lifecycle_mutex_);
    // Sole activation/linearization point for Start.
    transport_ = &transport;
    queryable_ = std::move(queryable);
    subscriber_ = std::move(subscriber);
    state_ = state;
    state->Activate();
  }
  return Result<void>::Ok();
}

void StorageNode::Stop() noexcept {
  std::unique_lock operation_lock(operation_mutex_);
  std::shared_ptr<State> state;
  Queryable queryable;
  Subscription subscriber;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (!state_) return;
    state = std::move(state_);
    queryable = std::move(queryable_);
    subscriber = std::move(subscriber_);
  }

  state->DeactivateAndWait();
  subscriber = Subscription{};
  queryable = Queryable{};
}

bool StorageNode::IsStarted() const noexcept {
  std::scoped_lock lock(lifecycle_mutex_);
  return state_ != nullptr;
}

void StorageNode::OnSample(const std::shared_ptr<State>& state, const TransportSample& sample) {
  auto lease = state->Enter();
  if (!lease) return;
  try {

    const auto parsed = ParseKey(state->prefix, sample.key);
    if (!parsed || parsed->kind != KeyKind::Base || parsed->is_batch) {
      EmitLog(state->log_sink, LogLevel::kWarning, kNodeComponent, kUnsupportedSubscriberKey);
      return;
    }

    if (sample.kind == TransportSample::Kind::Delete) {
      if (!state->engine->Delete(parsed->relative_key)) {
        EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberDeleteFailed);
      }
      return;
    }

    Bytes value = sample.payload;
    std::vector<std::byte> wrapped;
    if (sample.encoding.id != Encoding::kSitosV1) {
      EmitLog(state->log_sink, LogLevel::kWarning, kNodeComponent, kUnknownSubscriberEncoding);
      auto bytes = std::vector<std::byte>(sample.payload.begin(), sample.payload.end());
      wrapped = ParamValue(std::move(bytes)).Encode();
      value = wrapped;
    }
    if (!state->engine->Put(parsed->relative_key, value)) {
      EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberPutFailed);
    }
  } catch (...) {
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed);
  }
}

void StorageNode::OnQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  auto lease = state->Enter();
  if (!lease) return;

  try {
    auto parsed = ParseStorageQuery(state->prefix, query.keyexpr);
    if (!parsed) return;

    const Encoding encoding = SitosEncoding();
    auto reply = [state, &query, encoding](std::string_view key, Bytes value) {
      const std::string full_key = MakeReplyKey(state->prefix, key);
      return query.Reply(full_key, value, encoding).IsOk();
    };

    if (!parsed->is_list) {
      state->engine->Get(parsed->relative_key, reply);
      return;
    }

    state->engine->List(parsed->relative_key, reply);
  } catch (...) {
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, "query callback exception");
  }
}

}  // namespace sitos
