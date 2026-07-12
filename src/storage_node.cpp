// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode base queryable routing.

#include "sitos/storage_node.hpp"

#include <utility>

#include "sitos/key.hpp"

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

}  // namespace

std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix,
                                               std::string_view keyexpr) {
  if (!IsValidPrefix(prefix)) return std::nullopt;

  auto rest = StripPrefix(prefix, keyexpr);
  if (!rest || !rest->starts_with("base/")) return std::nullopt;
  std::string_view relative = rest->substr(5);
  if (relative.empty()) return std::nullopt;

  if (relative == "**") return StorageQuery{true, {}};

  constexpr std::string_view kSelectorSuffix = "/**";
  if (relative.ends_with(kSelectorSuffix)) {
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
  if (transport_ == nullptr) return Result<void>::Err(InvalidArgument());
  return Start(std::move(engine), *transport_, std::move(config));
}

Result<void> StorageNode::Start(std::shared_ptr<StorageEngine> engine, Transport& transport,
                                Config config) {
  if (state_ != nullptr) return Result<void>::Err(OperationInProgress());
  if (!engine || !IsValidPrefix(config.prefix)) {
    return Result<void>::Err(InvalidArgument());
  }

  auto state = std::make_shared<State>(std::move(engine), std::move(config.prefix));
  const std::string queryable_key = state->prefix + "/**";
  queryable_ = transport.DeclareQueryable(
      queryable_key, [state](TransportQuery& query) { OnQuery(state, query); });
  transport_ = &transport;
  state_ = std::move(state);
  return Result<void>::Ok();
}

void StorageNode::Stop() noexcept {
  if (state_ != nullptr) state_->active.store(false, std::memory_order_relaxed);
  queryable_ = Queryable{};
  state_.reset();
}

void StorageNode::OnQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  if (!state->active.load(std::memory_order_relaxed)) return;

  auto parsed = ParseStorageQuery(state->prefix, query.keyexpr);
  if (!parsed) return;

  const Encoding encoding = SitosEncoding();
  auto reply = [&](std::string_view key, Bytes value) {
    const std::string full_key = MakeReplyKey(state->prefix, key);
    return query.Reply(full_key, value, encoding).IsOk();
  };

  if (!parsed->is_list) {
    state->engine->Get(parsed->relative_key, reply);
    return;
  }

  state->engine->List(parsed->relative_key, reply);
}

}  // namespace sitos
