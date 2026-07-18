// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Thread-safe synchronous client operations against a StorageNode.

#include "sitos/param_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sitos {
namespace {

Result<void> InvalidKey(std::string_view message) {
  return Result<void>::Err(Status::InvalidKey, std::string(message));
}

Result<void> InvalidArgument(std::string_view message) {
  return Result<void>::Err(Status::InvalidArgument, std::string(message));
}

std::string ScopePath(const Scope& scope) {
  switch (scope.kind) {
    case ScopeKind::Base:
      return "base";
    case ScopeKind::Session:
      return "session/" + scope.sid;
    case ScopeKind::Snap:
      return "snap/" + scope.sid;
  }
  return {};
}

bool ContainsWhitespaceOrWildcard(std::string_view value) {
  for (char c : value) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == '*' || c == '?') return true;
  }
  return false;
}

bool ContainsBatchSegment(std::string_view value) {
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find('/', start);
    const std::size_t length = end == std::string_view::npos ? value.size() - start : end - start;
    if (value.substr(start, length) == ":batch") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

std::string BuildScopeQuery(std::string_view prefix, std::string_view scope_path,
                            std::string_view safe_parent) {
  std::string result;
  result.reserve(prefix.size() + scope_path.size() + safe_parent.size() + 5);
  result.append(prefix);
  result.push_back('/');
  result.append(scope_path);
  result.push_back('/');
  if (!safe_parent.empty()) {
    result.append(safe_parent);
    result.append("/**");
  } else {
    result.append("**");
  }
  return result;
}

std::string SafeParent(std::string_view requested_prefix) {
  if (requested_prefix.empty() || requested_prefix.ends_with('/')) {
    if (requested_prefix.empty()) return {};
    return std::string(requested_prefix.substr(0, requested_prefix.size() - 1));
  }
  const std::size_t slash = requested_prefix.rfind('/');
  if (slash == std::string_view::npos) return {};
  return std::string(requested_prefix.substr(0, slash));
}

bool IsUnderSafeParent(std::string_view relative, std::string_view safe_parent) {
  if (safe_parent.empty()) return true;
  return relative == safe_parent ||
         (relative.size() > safe_parent.size() && relative.starts_with(safe_parent) &&
          relative[safe_parent.size()] == '/');
}

bool IsExpectedKind(const ParsedKey& parsed, const Scope& scope) {
  switch (scope.kind) {
    case ScopeKind::Base:
      return parsed.kind == KeyKind::Base;
    case ScopeKind::Session:
      return parsed.kind == KeyKind::Session && parsed.sid == scope.sid;
    case ScopeKind::Snap:
      return parsed.kind == KeyKind::Snapshot && parsed.sid == scope.sid;
  }
  return false;
}

Result<ParamValue> DecodeReply(std::string_view expected_key, std::string_view actual_key,
                               std::span<const std::byte> payload, const Encoding& encoding) {
  if (actual_key != expected_key) {
    return Result<ParamValue>::Err(Status::Error, "transport returned an unexpected key");
  }
  if (encoding.id != Encoding::kSitosV1) {
    return Result<ParamValue>::Err(Status::Error, "transport returned an unexpected encoding");
  }
  auto decoded = ParamValue::Decode(payload);
  if (!decoded.has_value()) {
    return Result<ParamValue>::Err(Status::Error, "transport returned malformed payload");
  }
  return Result<ParamValue>::Ok(std::move(*decoded));
}

Result<void> DecodeListReply(std::string_view config_prefix, const Scope& scope,
                             std::string_view safe_parent, std::string_view prefix,
                             std::string_view full_key, std::span<const std::byte> payload,
                             const Encoding& encoding,
                             std::vector<std::pair<std::string, ParamValue>>& values) {
  auto parsed = ParseKey(config_prefix, full_key);
  if (!parsed || !IsExpectedKind(*parsed, scope) || parsed->is_batch ||
      !IsUnderSafeParent(parsed->relative_key, safe_parent)) {
    return Result<void>::Err(Status::Error, "transport returned an invalid list key");
  }
  if (!prefix.empty() && !parsed->relative_key.starts_with(prefix)) return Result<void>::Ok();
  if (encoding.id != Encoding::kSitosV1) {
    return Result<void>::Err(Status::Error, "transport returned an unexpected encoding");
  }
  auto decoded = ParamValue::Decode(payload);
  if (!decoded.has_value()) {
    return Result<void>::Err(Status::Error, "transport returned malformed payload");
  }
  values.emplace_back(parsed->relative_key, std::move(*decoded));
  return Result<void>::Ok();
}

}  // namespace

Result<ParamStore> ParamStore::Open(ClientConfig config) {
  auto config_result = ValidateClientConfig(config);
  if (!config_result.IsOk()) return Result<ParamStore>::ErrFrom(config_result);

  std::optional<std::string_view> json;
  if (config.zenoh_config_json.has_value()) json = *config.zenoh_config_json;
  auto transport_result = OpenZenohTransport(json);
  if (!transport_result.IsOk()) return Result<ParamStore>::ErrFrom(transport_result);
  std::shared_ptr<Transport> transport(std::move(transport_result).Value());
  return Result<ParamStore>::Ok(ParamStore(std::move(transport), std::move(config)));
}

Result<ParamStore> ParamStore::Open(std::shared_ptr<Transport> transport, ClientConfig config) {
  if (!transport) return Result<ParamStore>::Err(Status::InvalidArgument, "null transport");
  auto config_result = ValidateClientConfig(config);
  if (!config_result.IsOk()) return Result<ParamStore>::ErrFrom(config_result);
  if (config.zenoh_config_json.has_value()) {
    return Result<ParamStore>::Err(Status::InvalidArgument,
                                   "injected transport cannot apply zenoh configuration");
  }
  return Result<ParamStore>::Ok(ParamStore(std::move(transport), std::move(config)));
}

Result<Scope> ParamStore::ParseAndValidateScope(std::string_view scope) {
  auto parsed = ParseScope(scope);
  if (!parsed.has_value()) {
    return Result<Scope>::Err(Status::InvalidKey, "invalid scope");
  }
  return Result<Scope>::Ok(std::move(*parsed));
}

Result<void> ParamStore::ValidateUserKey(std::string_view key) {
  if (!IsValidKey(key)) return InvalidKey("invalid key");
  return Result<void>::Ok();
}

Result<void> ParamStore::ValidateListPrefix(std::string_view prefix) {
  if (prefix.empty()) return Result<void>::Ok();
  if (prefix.front() == '/' || prefix.find("//") != std::string_view::npos ||
      ContainsWhitespaceOrWildcard(prefix) || ContainsBatchSegment(prefix)) {
    return InvalidKey("invalid list prefix");
  }
  if (prefix.back() == '/') {
    if (prefix.size() == 1 || !IsValidPrefix(prefix.substr(0, prefix.size() - 1))) {
      return InvalidKey("invalid list prefix");
    }
    return Result<void>::Ok();
  }
  if (!IsValidKey(prefix)) return InvalidKey("invalid list prefix");
  return Result<void>::Ok();
}

Result<void> ParamStore::Put(std::string_view scope, std::string_view key,
                             const ParamValue& value) {
  if (!transport_) return InvalidArgument("moved-from ParamStore");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<void>::ErrFrom(parsed_scope);
  auto key_result = ValidateUserKey(key);
  if (!key_result.IsOk()) return key_result;
  if (parsed_scope.Value().kind == ScopeKind::Snap) {
    return Result<void>::Err(Status::ReadOnly, "snapshot scope is read-only");
  }
  auto full_key = BuildKey(config_.prefix, scope, key);
  if (!full_key.has_value()) return InvalidKey("invalid parameter key");
  auto payload = value.Encode();
  auto result =
      transport_->Put(*full_key, payload, Encoding{std::string(Encoding::kSitosV1)}, PutOptions{});
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  return Result<void>::Ok();
}

Result<void> ParamStore::PutBatch(std::string_view scope, std::span<const BatchEntry> entries) {
  if (!transport_) return InvalidArgument("moved-from ParamStore");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<void>::ErrFrom(parsed_scope);
  if (parsed_scope.Value().kind == ScopeKind::Snap) {
    return Result<void>::Err(Status::ReadOnly, "snapshot scope is read-only");
  }
  if (!BuildBatchKey(config_.prefix, scope).has_value()) {
    return InvalidKey("invalid batch scope");
  }
  for (const auto& entry : entries) {
    auto key_result = ValidateUserKey(entry.key);
    if (!key_result.IsOk()) return key_result;
  }
  if (entries.empty()) return Result<void>::Ok();

  auto payload = EncodeBatch(entries);
  auto key = BuildBatchKey(config_.prefix, scope);
  auto result =
      transport_->Put(*key, payload, Encoding{std::string(Encoding::kSitosV1Batch)}, PutOptions{});
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  return Result<void>::Ok();
}

Result<void> ParamStore::Delete(std::string_view scope, std::string_view key) {
  if (!transport_) return InvalidArgument("moved-from ParamStore");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<void>::ErrFrom(parsed_scope);
  auto key_result = ValidateUserKey(key);
  if (!key_result.IsOk()) return key_result;
  if (parsed_scope.Value().kind == ScopeKind::Snap) {
    return Result<void>::Err(Status::ReadOnly, "snapshot scope is read-only");
  }
  if (parsed_scope.Value().kind == ScopeKind::Session) {
    return InvalidKey("session delete is not supported");
  }
  auto full_key = BuildKey(config_.prefix, scope, key);
  if (!full_key.has_value()) return InvalidKey("invalid parameter key");
  auto result = transport_->Delete(*full_key, PutOptions{});
  if (!result.IsOk()) return Result<void>::ErrFrom(result);
  return Result<void>::Ok();
}

Result<ParamValue> ParamStore::Get(std::string_view scope, std::string_view key) {
  if (!transport_) return Result<ParamValue>::Err(Status::InvalidArgument, "moved-from ParamStore");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<ParamValue>::ErrFrom(parsed_scope);
  auto key_result = ValidateUserKey(key);
  if (!key_result.IsOk()) return Result<ParamValue>::ErrFrom(key_result);
  auto full_key = BuildKey(config_.prefix, scope, key);
  if (!full_key.has_value()) {
    return Result<ParamValue>::Err(Status::InvalidKey, "invalid parameter key");
  }

  std::optional<Result<ParamValue>> callback_result;
  auto transport_result = transport_->Get(
      *full_key,
      [&callback_result, &full_key](std::string_view actual_key,
                                    std::span<const std::byte> payload, Encoding encoding) {
        if (callback_result.has_value()) return false;
        callback_result = DecodeReply(*full_key, actual_key, payload, encoding);
        return callback_result->IsOk();
      },
      config_.query_timeout);
  if (!transport_result.IsOk()) return Result<ParamValue>::ErrFrom(transport_result);
  if (callback_result.has_value()) return std::move(*callback_result);
  return Result<ParamValue>::Err(Status::NotFound, "parameter not found");
}

Result<bool> ParamStore::Contains(std::string_view scope, std::string_view key) {
  auto result = Get(scope, key);
  if (!result.IsOk() && result.StatusCode() == Status::NotFound) return Result<bool>::Ok(false);
  if (!result.IsOk()) return Result<bool>::ErrFrom(result);
  return Result<bool>::Ok(true);
}

Result<void> ParamStore::List(std::string_view scope, std::string_view prefix,
                              const ListSink& sink) {
  if (!transport_) return InvalidArgument("moved-from ParamStore");
  if (!sink) return InvalidArgument("null list sink");
  auto parsed_scope = ParseAndValidateScope(scope);
  if (!parsed_scope.IsOk()) return Result<void>::ErrFrom(parsed_scope);
  auto prefix_result = ValidateListPrefix(prefix);
  if (!prefix_result.IsOk()) return prefix_result;

  const std::string scope_path = ScopePath(parsed_scope.Value());
  const std::string safe_parent = SafeParent(prefix);
  const std::string selector = BuildScopeQuery(config_.prefix, scope_path, safe_parent);
  std::vector<std::pair<std::string, ParamValue>> values;
  std::optional<Result<void>> callback_error;

  auto transport_result = transport_->Get(
      selector,
      [this, parsed_scope, safe_parent, prefix, &callback_error, &values](
          std::string_view full_key, std::span<const std::byte> payload, Encoding encoding) {
        auto reply = DecodeListReply(config_.prefix, parsed_scope.Value(), safe_parent, prefix,
                                     full_key, payload, encoding, values);
        if (!reply.IsOk()) {
          callback_error = std::move(reply);
          return false;
        }
        return true;
      },
      config_.query_timeout);
  if (!transport_result.IsOk()) return Result<void>::ErrFrom(transport_result);
  if (callback_error.has_value()) return *callback_error;

  std::sort(values.begin(), values.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  for (const auto& [relative_key, value] : values) {
    if (!sink(relative_key, value)) break;
  }
  return Result<void>::Ok();
}

}  // namespace sitos
