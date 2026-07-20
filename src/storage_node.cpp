// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for base, session, and snapshot
// scopes, plus session lifecycle management.

#include "sitos/storage_node.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
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

std::error_code SessionAlreadyExists() {
  return std::make_error_code(std::errc::file_exists);
}

std::error_code NoSuchSession() {
  return std::make_error_code(std::errc::no_such_file_or_directory);
}

std::optional<std::string_view> StripPrefix(std::string_view prefix,
                                            std::string_view keyexpr) {
  if (keyexpr.size() <= prefix.size() || !keyexpr.starts_with(prefix) ||
      keyexpr[prefix.size()] != '/') {
    return std::nullopt;
  }
  return keyexpr.substr(prefix.size() + 1);
}

// Splits `rest` at the first '/' into (head, tail). Returns std::nullopt if
// there is no '/'.
std::optional<std::pair<std::string_view, std::string_view>> SplitFirst(std::string_view rest) {
  std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  return std::pair{rest.substr(0, slash), rest.substr(slash + 1)};
}

// Builds a reply key <prefix>/<scope_path>/<relative_key>, where scope_path is
// "base", "session/<sid>", or "snap/<sid>".
std::string MakeReplyKey(std::string_view prefix, std::string_view scope_path,
                         std::string_view relative_key) {
  std::string key;
  key.reserve(prefix.size() + scope_path.size() + relative_key.size() + 2);
  key.append(prefix);
  key.push_back('/');
  key.append(scope_path);
  key.push_back('/');
  key.append(relative_key);
  return key;
}

Encoding SitosEncoding() { return Encoding{std::string(Encoding::kSitosV1)}; }

// Formats the current time as an ISO-8601 UTC timestamp, e.g. 2026-07-14T01:23:45Z.
std::string NowIso8601() {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  std::ostringstream result;
  result << std::setfill('0') << std::setw(4) << tm.tm_year + 1900 << '-' << std::setw(2)
         << tm.tm_mon + 1 << '-' << std::setw(2) << tm.tm_mday << 'T' << std::setw(2)
         << tm.tm_hour << ':' << std::setw(2) << tm.tm_min << ':' << std::setw(2) << tm.tm_sec
         << 'Z';
  return result.str();
}

// Parses a scope-relative selector (the part after base/, session/<sid>/, or
// snap/<sid>/) into an exact key or a terminal List selector.
std::optional<StorageQuery> ParseRelativeSelector(std::string_view relative) {
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

// Replies to a get/List against a resolved reader, rebuilding reply keys under
// the given scope path.
void ReplyFromReader(const StorageReader& reader, const StorageQuery& selector,
                     std::string_view prefix, std::string_view scope_path,
                     TransportQuery& query) {
  const Encoding encoding = SitosEncoding();
  auto reply = [prefix, scope_path, &query, encoding](std::string_view key, Bytes value) {
    const std::string full_key = MakeReplyKey(prefix, scope_path, key);
    return query.Reply(full_key, value, encoding).IsOk();
  };
  if (!selector.is_list) {
    reader.Get(selector.relative_key, reply);
    return;
  }
  reader.List(selector.relative_key, reply);
}

constexpr std::string_view kNodeComponent = "node";
constexpr std::string_view kUnsupportedSubscriberKey = "unsupported subscriber key";
constexpr std::string_view kUnknownSubscriberEncoding =
    "unknown subscriber encoding; wrapped as bytes";
constexpr std::string_view kSubscriberPutFailed = "subscriber PUT failed";
constexpr std::string_view kSubscriberDeleteFailed = "subscriber DELETE failed";
constexpr std::string_view kSubscriberCallbackFailed = "subscriber callback exception";
constexpr std::string_view kMalformedBatchPayload = "malformed batch payload";
constexpr std::string_view kInvalidBatchEntry = "invalid batch entry key";
constexpr std::string_view kInvalidBatchOperation = "invalid batch operation or encoding";
constexpr std::string_view kReadOnlySnapshotKey = "read-only snapshot key";
constexpr std::string_view kUnknownSession = "unknown session";
constexpr std::string_view kQueryCallbackFailed = "query callback exception";

struct SubscriberDiagnostic {
  LogLevel level;
  std::string_view message;
};

using SubscriberDiagnostics = std::vector<SubscriberDiagnostic>;

bool IsBatchPut(const TransportSample& sample) {
  return sample.kind == TransportSample::Kind::Put &&
         sample.encoding.id == Encoding::kSitosV1Batch;
}

// Applies a put/delete sample to a target engine (base engine or session
// overlay), mirroring the wire-encoding rules for base writes. Diagnostics are
// retained until the subscriber sequencer is released, so an injected sink is
// never called while node application state is locked.
void ApplyWrite(SubscriberDiagnostics& diagnostics, StorageEngine& target,
                std::string_view relative_key, const TransportSample& sample) {
  if (sample.kind == TransportSample::Kind::Delete) {
    if (!target.Delete(relative_key)) {
      diagnostics.push_back({LogLevel::kError, kSubscriberDeleteFailed});
    }
    return;
  }

  Bytes value = sample.payload;
  std::vector<std::byte> wrapped;
  if (sample.encoding.id != Encoding::kSitosV1) {
    diagnostics.push_back({LogLevel::kWarning, kUnknownSubscriberEncoding});
    auto bytes = std::vector<std::byte>(sample.payload.begin(), sample.payload.end());
    wrapped = ParamValue(std::move(bytes)).Encode();
    value = wrapped;
  }
  if (!target.Put(relative_key, value)) {
    diagnostics.push_back({LogLevel::kError, kSubscriberPutFailed});
  }
}

struct EncodedBatchEntry {
  std::string key;
  std::vector<std::byte> value;
};

// Validates and materializes every batch entry before the first engine write.
// StorageEngine has no transactional API: failed writes are logged and later
// validated entries are still attempted in their encoded order.
void ApplyBatch(SubscriberDiagnostics& diagnostics, StorageEngine& target,
                std::span<const std::byte> payload) {
  auto decoded = DecodeBatch(payload);
  if (!decoded) {
    diagnostics.push_back({LogLevel::kWarning, kMalformedBatchPayload});
    return;
  }

  std::vector<EncodedBatchEntry> entries;
  entries.reserve(decoded->size());
  for (const auto& entry : *decoded) {
    if (!IsValidKey(entry.key)) {
      diagnostics.push_back({LogLevel::kWarning, kInvalidBatchEntry});
      return;
    }
    entries.push_back({entry.key, entry.value.Encode()});
  }

  for (const auto& entry : entries) {
    if (!target.Put(entry.key, entry.value)) {
      diagnostics.push_back({LogLevel::kError, kSubscriberPutFailed});
    }
  }
}

void EmitDiagnostics(const std::shared_ptr<LogSink>& log_sink,
                     const SubscriberDiagnostics& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    EmitLog(log_sink, diagnostic.level, kNodeComponent, diagnostic.message);
  }
}

}  // namespace

std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix,
                                               std::string_view keyexpr) {
  if (!IsValidPrefix(prefix)) return std::nullopt;

  auto rest = StripPrefix(prefix, keyexpr);
  if (!rest || !rest->starts_with("base/")) return std::nullopt;
  return ParseRelativeSelector(rest->substr(5));
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
  if (!queryable_result.IsOk()) return Result<void>::ErrFrom(queryable_result);
  Queryable queryable = std::move(queryable_result).Value();

  auto subscriber_result = transport.DeclareSubscriber(
      declaration_key, [state](const TransportSample& sample) { OnSample(state, sample); });
  if (!subscriber_result.IsOk()) return Result<void>::ErrFrom(subscriber_result);
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

Result<void> StorageNode::CreateSession(std::string_view sid) {
  if (!IsValidSessionId(sid)) return Result<void>::Err(InvalidArgument());

  std::shared_ptr<State> state;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    state = state_;
  }
  if (!state) return Result<void>::Err(InvalidArgument());
  // Enroll in the callback gate so a concurrent Stop() cannot tear the node down
  // mid-operation; a node already quiescing rejects the request.
  auto lease = state->Enter();
  if (!lease) return Result<void>::Err(InvalidArgument());

  // Take the snapshot before locking the session table: TakeSnapshot() may be
  // O(n) and the engine is independently thread-safe.
  auto snapshot = state->engine->TakeSnapshot();
  auto overlay = std::make_shared<InMemoryEngine>();
  SessionMeta meta{NowIso8601()};

  const std::string key(sid);
  std::unique_lock lock(state->session_mutex);
  if (state->sessions.contains(key)) return Result<void>::Err(SessionAlreadyExists());
  state->snapshots.emplace(key, std::move(snapshot));
  state->overlays.emplace(key, std::move(overlay));
  state->sessions.emplace(key, std::move(meta));
  return Result<void>::Ok();
}

Result<void> StorageNode::CloseSession(std::string_view sid) {
  std::shared_ptr<State> state;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    state = state_;
  }
  if (!state) return Result<void>::Err(InvalidArgument());
  auto lease = state->Enter();
  if (!lease) return Result<void>::Err(InvalidArgument());

  const std::string key(sid);
  std::unique_lock lock(state->session_mutex);
  auto it = state->sessions.find(key);
  if (it == state->sessions.end()) return Result<void>::Err(NoSuchSession());
  state->sessions.erase(it);
  state->snapshots.erase(key);
  state->overlays.erase(key);
  return Result<void>::Ok();
}

std::vector<std::string> StorageNode::ActiveSessions() const {
  std::shared_ptr<State> state;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    state = state_;
  }
  if (!state) return {};
  auto lease = state->Enter();
  if (!lease) return {};

  std::shared_lock lock(state->session_mutex);
  std::vector<std::string> result;
  result.reserve(state->sessions.size());
  for (const auto& [id, meta] : state->sessions) {
    (void)meta;
    result.push_back(id);
  }
  return result;
}

void StorageNode::OnSample(const std::shared_ptr<State>& state, const TransportSample& sample) {
  auto lease = state->Enter();
  if (!lease) return;
  try {
    SubscriberDiagnostics diagnostics;
    {
      // The gate lease is acquired first. Serializing the entire subscriber
      // path prevents an ordinary write from becoming visible between batch
      // entries. Diagnostics are emitted only after releasing this lock.
      std::scoped_lock application_lock(state->subscriber_mutex);
      const auto parsed = ParseKey(state->prefix, sample.key);
      if (!parsed) {
        diagnostics.push_back({LogLevel::kWarning, kUnsupportedSubscriberKey});
      } else if (parsed->is_batch) {
        // ParseKey only marks Base and Session paths as batch paths.
        if (!IsBatchPut(sample)) {
          diagnostics.push_back({LogLevel::kWarning, kInvalidBatchOperation});
        } else if (parsed->kind == KeyKind::Base) {
          ApplyBatch(diagnostics, *state->engine, sample.payload);
        } else {
          std::shared_ptr<StorageEngine> overlay;
          {
            std::shared_lock lock(state->session_mutex);
            auto it = state->overlays.find(parsed->sid);
            if (it != state->overlays.end()) overlay = it->second;
          }
          if (!overlay) {
            diagnostics.push_back({LogLevel::kWarning, kUnknownSession});
          } else {
            ApplyBatch(diagnostics, *overlay, sample.payload);
          }
        }
      } else {
        switch (parsed->kind) {
          case KeyKind::Base:
            ApplyWrite(diagnostics, *state->engine, parsed->relative_key, sample);
            break;
          case KeyKind::Session: {
            std::shared_ptr<StorageEngine> overlay;
            {
              std::shared_lock lock(state->session_mutex);
              auto it = state->overlays.find(parsed->sid);
              if (it != state->overlays.end()) overlay = it->second;
            }
            if (!overlay) {
              diagnostics.push_back({LogLevel::kWarning, kUnknownSession});
            } else {
              ApplyWrite(diagnostics, *overlay, parsed->relative_key, sample);
            }
            break;
          }
          case KeyKind::Snapshot:
            diagnostics.push_back({LogLevel::kWarning, kReadOnlySnapshotKey});
            break;
          default:  // MetaSession, MetaAck (#14): not writable via subscriber.
            diagnostics.push_back({LogLevel::kWarning, kUnsupportedSubscriberKey});
            break;
        }
      }
    }
    EmitDiagnostics(state->log_sink, diagnostics);
  } catch (...) {
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed);
  }
}

void StorageNode::OnQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  auto lease = state->Enter();
  if (!lease) return;

  try {
    auto rest = StripPrefix(state->prefix, query.keyexpr);
    if (!rest) return;
    auto split = SplitFirst(*rest);
    if (!split) return;
    const auto [head, tail] = *split;

    if (head == "base") {
      if (auto selector = ParseRelativeSelector(tail)) {
        ReplyFromReader(*state->engine, *selector, state->prefix, "base", query);
      }
    } else if (head == "snap" || head == "session") {
      ReplyScopedQuery(state, head, tail, query);
    } else if (head == "meta") {
      ReplyMetaQuery(state, query);
    }
  } catch (...) {
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kQueryCallbackFailed);
  }
}

void StorageNode::ReplyScopedQuery(const std::shared_ptr<State>& state, std::string_view scope,
                                   std::string_view tail, TransportQuery& query) {
  auto sid_split = SplitFirst(tail);
  if (!sid_split) return;
  const auto [sid, relative] = *sid_split;
  if (!IsValidSessionId(sid)) return;
  auto selector = ParseRelativeSelector(relative);
  if (!selector) return;

  std::shared_ptr<const StorageReader> reader;
  {
    std::shared_lock lock(state->session_mutex);
    if (scope == "snap") {
      auto it = state->snapshots.find(std::string(sid));
      if (it != state->snapshots.end()) reader = it->second;
    } else {
      auto it = state->overlays.find(std::string(sid));
      if (it != state->overlays.end()) reader = it->second;
    }
  }
  if (!reader) return;  // Unknown sid: 0 replies.

  const std::string scope_path = std::string(scope) + "/" + std::string(sid);
  ReplyFromReader(*reader, *selector, state->prefix, scope_path, query);
}

void StorageNode::ReplyMetaQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  auto parsed = ParseKey(state->prefix, query.keyexpr);
  // MetaAck (#14) is out of scope; only meta/session is answered here.
  if (!parsed || parsed->kind != KeyKind::MetaSession) return;

  std::string json;
  {
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(parsed->sid);
    if (it == state->sessions.end()) return;  // Unknown sid: 0 replies.
    json = R"({"state":"active","created_at":")";
    json += it->second.created_at;
    json += R"("})";
  }
  const auto payload = ParamValue(json).Encode();
  query.Reply(query.keyexpr, payload, SitosEncoding());
}

}  // namespace sitos
