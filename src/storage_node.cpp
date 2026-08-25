// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for base, session, and snapshot
// scopes, plus session lifecycle management.

#include "sitos/storage_node.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <new>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "ack_registry.hpp"
#include "fence_internal.hpp"
#include "fence_test_access.hpp"
#include "sitos/ack.hpp"
#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/key.hpp"
#include "sitos/param_value.hpp"

namespace sitos {
using namespace fence_internal;

// Diagnostics are retained until the subscriber sequencer is released, so an
// injected sink is never called while node application state is locked.
struct SubscriberDiagnostic {
  LogLevel level;
  std::string_view message;
};

// Per-sample application progress observed by the ADR-0028 completion guard:
// whether any engine call was invoked (OutcomeUnknown vs. Error on an exception)
// and the confirmed batch prefix / entry being applied.
struct AckApplyProgress {
  AckOperationKind kind = AckOperationKind::Put;
  bool engine_invoked = false;
  std::uint32_t applied_count = 0;
  std::uint32_t current_index = 0;
};

struct FenceApplyProgress {
  bool application_started = false;
  bool completion_retained = false;
};

namespace {

std::error_code InvalidArgument() { return std::make_error_code(std::errc::invalid_argument); }

std::error_code OperationInProgress() {
  return std::make_error_code(std::errc::operation_in_progress);
}

std::error_code SessionAlreadyExists() { return std::make_error_code(std::errc::file_exists); }

std::error_code NoSuchSession() {
  return std::make_error_code(std::errc::no_such_file_or_directory);
}

std::optional<std::string_view> StripPrefix(std::string_view prefix, std::string_view keyexpr) {
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
  const std::time_t seconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Parses a scope-relative selector (the part after base/, session/<sid>/, or
// snap/<sid>/) into an exact key or a terminal List selector.
std::optional<StorageQuery> ParseRelativeSelector(std::string_view relative) {
  if (relative.empty()) return std::nullopt;

  if (relative == "**") return StorageQuery{true, {}};

  if (constexpr std::string_view kSelectorSuffix = "/**"; relative.ends_with(kSelectorSuffix)) {
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
                     std::string_view prefix, std::string_view scope_path, TransportQuery& query) {
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
constexpr std::string_view kBufferUnsupported = "unsupported buffer operation";
constexpr std::string_view kBufferCapabilityDisabled = "buffer capability disabled";
constexpr std::string_view kBufferEncodingRejected = "buffer encoding rejected";
constexpr std::string_view kBufferPutConflict = "durable buffer PUT conflicts with existing value";
constexpr std::string_view kBufferReadBeforePutFailed = "durable buffer read before PUT failed";
constexpr std::string_view kBufferPutFailed = "durable buffer PUT failed";
constexpr std::string_view kBufferQueryFailed = "durable buffer query failed";
constexpr std::string_view kMalformedAckAttachment = "malformed ack attachment; sample rejected";
constexpr std::string_view kAckTokenCollision = "ack token collision; sample rejected";
constexpr std::string_view kAckLaneReentry = "ack lane reentry; sample rejected";
constexpr std::string_view kSubscriberReentry = "subscriber reentry; sample rejected";
constexpr std::string_view kInvalidAckPayload = "invalid sitos.v1 payload; sample rejected";
constexpr std::string_view kAckUnsupportedOperation =
    "acknowledgement not supported for this operation; sample rejected";

using SubscriberDiagnostics = std::vector<SubscriberDiagnostic>;

// Holds the serialized application lane: takes subscriber_mutex and records the
// owning thread so that same-thread reentry can be detected without blocking.
class ApplicationLaneLock {
 public:
  ApplicationLaneLock(std::mutex& lane_mutex, std::atomic<std::thread::id>& owner)
      : owner_(owner), lock_(lane_mutex) {
    owner_.store(std::this_thread::get_id());
  }
  ~ApplicationLaneLock() { owner_.store(std::thread::id{}); }
  ApplicationLaneLock(const ApplicationLaneLock&) = delete;
  ApplicationLaneLock& operator=(const ApplicationLaneLock&) = delete;

 private:
  std::atomic<std::thread::id>& owner_;
  std::scoped_lock<std::mutex> lock_;
};

AckResultV1 MakeAckResult(AckOperationKind kind, Status status, std::uint32_t applied_count,
                          std::uint32_t failed_index) {
  return AckResultV1{kind,         status, AckDurability::Applied, applied_count,
                     failed_index, 0,      kAckNoFailedSequence,   ""};
}

// Definite rejection or uncertain outcome for a Put: applied_count 0, failed_index 0.
AckResultV1 PutFailure(Status status) { return MakeAckResult(AckOperationKind::Put, status, 0, 0); }

// ADR-0028: reentrant admission on a serialized lane is Status::Error without application.
AckResultV1 LaneBusyResult(AckOperationKind kind) {
  return MakeAckResult(kind, Status::Error, 0,
                       kind == AckOperationKind::Batch ? kAckNoFailedIndex : 0);
}

// Moves a Processing token to exactly one immutable Completed result, even when
// application throws after the claim (ADR-0028 RAII completion guard).
class AckCompletionGuard {
 public:
  AckCompletionGuard(AckRegistry& registry, AckToken token, AckApplyProgress& progress)
      : registry_(registry), token_(token), progress_(progress) {}
  ~AckCompletionGuard() {
    if (done_) return;
    // Before any engine invocation the failure is definite; afterwards the engine
    // contract cannot prove that no mutation occurred.
    if (!progress_.engine_invoked) {
      const std::uint32_t failed_index =
          progress_.kind == AckOperationKind::Batch ? kAckNoFailedIndex : 0;
      registry_.Complete(token_, MakeAckResult(progress_.kind, Status::Error, 0, failed_index));
      return;
    }
    const std::uint32_t failed_index =
        progress_.kind == AckOperationKind::Batch ? progress_.current_index : 0;
    registry_.Complete(token_, MakeAckResult(progress_.kind, Status::OutcomeUnknown,
                                             progress_.applied_count, failed_index));
  }
  AckCompletionGuard(const AckCompletionGuard&) = delete;
  AckCompletionGuard& operator=(const AckCompletionGuard&) = delete;

  void Finish(AckResultV1 result) {
    registry_.Complete(token_, std::move(result));
    done_ = true;
  }

 private:
  AckRegistry& registry_;
  AckToken token_;
  AckApplyProgress& progress_;
  bool done_ = false;
};

// A claimed Fence marker has the same immutable-completion obligation as an
// acknowledged write. Before the optional durability barrier is invoked, an
// unexpected exception is a definite local Error; the barrier path translates
// its own post-invocation uncertainty to OutcomeUnknown before Finish().
class FenceAckCompletionGuard {
 public:
  FenceAckCompletionGuard(AckRegistry& registry, AckToken token, AckResultV1 fallback) noexcept
      : registry_(registry), token_(token), fallback_(std::move(fallback)) {}
  ~FenceAckCompletionGuard() {
    if (!done_) registry_.Complete(token_, std::move(fallback_));
  }
  FenceAckCompletionGuard(const FenceAckCompletionGuard&) = delete;
  FenceAckCompletionGuard& operator=(const FenceAckCompletionGuard&) = delete;

  void Finish(AckResultV1 result) {
    registry_.Complete(token_, std::move(result));
    done_ = true;
  }

 private:
  AckRegistry& registry_;
  AckToken token_;
  AckResultV1 fallback_;
  bool done_ = false;
};

bool IsBatchPut(const TransportSample& sample) {
  return sample.kind == TransportSample::Kind::Put && sample.encoding.id == Encoding::kSitosV1Batch;
}

bool IsBufferBytes(const TransportSample& sample) {
  return sample.kind == TransportSample::Kind::Put && sample.encoding.id == "zenoh/bytes";
}

enum class BufferWriteOutcome { Stored, Conflict, ReadFailed, WriteFailed };

BufferWriteOutcome ApplyDurableBufferWrite(StorageEngine& engine, const ParsedKey& parsed,
                                           const TransportSample& sample) {
  try {
    bool same = false;
    if (engine.Get(parsed.relative_key, [&same, &sample](std::string_view, Bytes value) {
          same = value.size() == sample.payload.size() &&
                 std::equal(value.begin(), value.end(), sample.payload.begin());
          return true;
        })) {
      return same ? BufferWriteOutcome::Stored : BufferWriteOutcome::Conflict;
    }
  } catch (...) {
    return BufferWriteOutcome::ReadFailed;
  }
  try {
    if (engine.Put(parsed.relative_key, sample.payload)) return BufferWriteOutcome::Stored;
  } catch (...) {
    return BufferWriteOutcome::WriteFailed;
  }
  return BufferWriteOutcome::WriteFailed;
}

// Applies a put/delete sample to a target engine (base engine or session
// overlay), mirroring the wire-encoding rules for base writes. Returns the
// typed ADR-0028 outcome: a boolean engine failure is OutcomeUnknown because the
// current StorageEngine contract cannot prove that no mutation occurred.
AckResultV1 ApplyWrite(SubscriberDiagnostics& diagnostics, StorageEngine& target,
                       std::string_view relative_key, const TransportSample& sample,
                       AckApplyProgress* progress) {
  if (sample.kind == TransportSample::Kind::Delete) {
    if (progress != nullptr) progress->engine_invoked = true;
    if (!target.Delete(relative_key)) {
      diagnostics.push_back({LogLevel::kError, kSubscriberDeleteFailed});
      return PutFailure(Status::OutcomeUnknown);
    }
    return MakeAckResult(AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
  }

  Bytes value = sample.payload;
  std::vector<std::byte> wrapped;
  if (sample.encoding.id != Encoding::kSitosV1) {
    diagnostics.push_back({LogLevel::kWarning, kUnknownSubscriberEncoding});
    auto bytes = std::vector<std::byte>(sample.payload.begin(), sample.payload.end());
    wrapped = ParamValue(std::move(bytes)).Encode();
    value = wrapped;
  } else if (progress != nullptr && !ParamValue::Decode(sample.payload).has_value()) {
    // ADR-0028: an acknowledged operation is decoded and validated completely before
    // its first engine mutation; a definite rejection creates a typed failure result
    // without mutating storage. Acknowledgement-free writes keep their existing
    // pass-through behavior, which is owned by the base/session write path.
    diagnostics.push_back({LogLevel::kWarning, kInvalidAckPayload});
    return PutFailure(Status::InvalidArgument);
  }
  if (progress != nullptr) progress->engine_invoked = true;
  if (!target.Put(relative_key, value)) {
    diagnostics.push_back({LogLevel::kError, kSubscriberPutFailed});
    return PutFailure(Status::OutcomeUnknown);
  }
  return MakeAckResult(AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
}

struct EncodedBatchEntry {
  std::string key;
  std::vector<std::byte> value;
};

// Validates and materializes every batch entry before the first engine write,
// then applies in caller order and stops at the first engine failure (ADR-0028;
// PutBatch is not transactional). The result reports the confirmed prefix in
// applied_count and the first failed entry in failed_index.
AckResultV1 ApplyBatch(SubscriberDiagnostics& diagnostics, StorageEngine& target,
                       std::span<const std::byte> payload, AckApplyProgress* progress) {
  auto decoded = DecodeBatch(payload);
  if (!decoded) {
    diagnostics.push_back({LogLevel::kWarning, kMalformedBatchPayload});
    return MakeAckResult(AckOperationKind::Batch, Status::InvalidArgument, 0, kAckNoFailedIndex);
  }

  std::vector<EncodedBatchEntry> entries;
  entries.reserve(decoded->size());
  for (std::size_t i = 0; i < decoded->size(); ++i) {
    const auto& entry = (*decoded)[i];
    if (!IsValidKey(entry.key)) {
      diagnostics.push_back({LogLevel::kWarning, kInvalidBatchEntry});
      return MakeAckResult(AckOperationKind::Batch, Status::InvalidArgument, 0,
                           static_cast<std::uint32_t>(i));
    }
    entries.push_back({entry.key, entry.value.Encode()});
  }

  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (progress != nullptr) {
      progress->current_index = static_cast<std::uint32_t>(i);
      progress->engine_invoked = true;
    }
    if (!target.Put(entries[i].key, entries[i].value)) {
      diagnostics.push_back({LogLevel::kError, kSubscriberPutFailed});
      return MakeAckResult(AckOperationKind::Batch, Status::OutcomeUnknown,
                           static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i));
    }
    if (progress != nullptr) progress->applied_count = static_cast<std::uint32_t>(i + 1);
  }
  return MakeAckResult(AckOperationKind::Batch, Status::Ok,
                       static_cast<std::uint32_t>(entries.size()), kAckNoFailedIndex);
}

void EmitDiagnostics(const std::shared_ptr<LogSink>& log_sink,
                     const SubscriberDiagnostics& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    EmitLog(log_sink, diagnostic.level, kNodeComponent, diagnostic.message);
  }
}

}  // namespace

std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix, std::string_view keyexpr) {
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
                                       std::move(config.log_sink),
                                       std::move(config.durable_buffer_engine_factory));
  state->ack_registry = std::make_shared<AckRegistry>();
  state->fence_dispatcher = transport.FenceDispatcher();
  state->fence_receiver_registry = std::make_shared<fence_internal::FenceReceiverRegistry>();
  const std::string declaration_key = state->prefix + "/**";
  auto queryable_result = transport.DeclareQueryable(
      declaration_key, [state](TransportQuery& query) { OnQuery(state, query); });
  if (!queryable_result.IsOk()) return Result<void>::ErrFrom(queryable_result);
  Queryable queryable = std::move(queryable_result).Value();

  auto subscriber_result = transport.DeclareSubscriber(
      declaration_key, [state](const TransportSample& sample) { DispatchSample(state, sample); });
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

  // Stop closes callback admission first. A marker that already claimed its
  // token therefore completes with the stop boundary status before quiescence,
  // while callbacks queued only in the Fence dispatcher fail State admission.
  // CreateSession operations admitted before this point are included in the
  // drain, so the subsequent Session snapshot is final.
  state->DeactivateAndWait();

  // Stop is a quiescence boundary for the entire Session generation. Extract
  // one existing map node at a time so this noexcept path performs no snapshot
  // allocation. Each record remains alive while its dispatch registrations and
  // active Session admissions drain.
  for (;;) {
    std::shared_ptr<SessionRecord> record;
    {
      std::unique_lock lock(state->session_mutex);
      if (state->sessions.empty()) break;
      auto node = state->sessions.extract(state->sessions.begin());
      record = std::move(node.mapped());
    }
    if (state->fence_dispatcher) {
      state->fence_dispatcher->CloseAndWait(record->fence_dispatch->durable);
      state->fence_dispatcher->CloseAndWait(record->fence_dispatch->ephemeral);
    }
    if (record->BeginClose()) record->WaitForAdmission();
    record->snapshot.reset();
    record->overlay.reset();
    record->durable_buffers.reset();
    record->metadata = {};
  }

  // ADR-0028: after callbacks have quiesced, Processing and Completed token state
  // is dropped; a later Start never recovers old results.
  state->ack_registry->Clear();
  state->fence_receiver_registry->Clear();

  subscriber = Subscription{};
  queryable = Queryable{};
}

bool StorageNode::IsStarted() const noexcept {
  std::scoped_lock lock(lifecycle_mutex_);
  return state_ != nullptr;
}

Result<void> StorageNode::CreateSession(std::string_view sid) {
  return CreateSession(sid, SessionOptions{});
}

Result<void> StorageNode::CreateSession(std::string_view sid, SessionOptions options) {
  if (!IsValidSessionId(sid)) return Result<void>::Err(InvalidArgument());

  std::shared_ptr<State> state;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    state = state_;
  }
  if (!state) return Result<void>::Err(InvalidArgument());
  std::function<void()> create_observer;
  {
    std::scoped_lock lock(state->test_observer_mutex);
    create_observer = state->create_session_entry_observer;
  }
  if (create_observer) create_observer();
  auto lease = state->Enter();
  if (!lease) return Result<void>::Err(InvalidArgument());
  return CreateSession(state, sid, options);
}

Result<void> StorageNode::CreateSession(const std::shared_ptr<State>& state, std::string_view sid,
                                        SessionOptions options) {
  const std::string key(sid);
  auto record = std::make_shared<SessionRecord>();
  // Every field visible to subscriber dispatch is immutable and initialized
  // before the Creating record is published in the Session table.
  record->options = options;
  record->generation_uuid = GenerateFenceUuid();
  record->fence_dispatch = std::make_shared<fence_internal::FenceSessionDispatch>();
  if (state->fence_dispatcher) {
    record->fence_dispatch->durable = state->fence_dispatcher->Register();
    record->fence_dispatch->ephemeral = state->fence_dispatcher->Register();
  }
  {
    std::unique_lock lock(state->fence_test_mutex);
    if (state->fence_test_gate_session_registration) {
      state->fence_test_session_registration_reached = true;
      state->fence_test_condition.notify_all();
      state->fence_test_condition.wait(
          lock, [state] { return state->fence_test_release_session_registration; });
    }
  }
  {
    std::unique_lock lock(state->session_mutex);
    if (auto it = state->sessions.find(key); it != state->sessions.end()) {
      if (!it->second->IsActive()) return Result<void>::Err(OperationInProgress());
      return Result<void>::Err(SessionAlreadyExists());
    }
    state->sessions.try_emplace(key, record);
  }

  struct SessionRollbackGuard {
    State* state;
    const std::string& key;
    std::shared_ptr<SessionRecord> record;
    bool active = true;

    SessionRollbackGuard(State* state_in, const std::string& key_in,
                         std::shared_ptr<SessionRecord> record_in)
        : state(state_in), key(key_in), record(std::move(record_in)) {}

    SessionRollbackGuard(const SessionRollbackGuard&) = delete;
    SessionRollbackGuard& operator=(const SessionRollbackGuard&) = delete;
    SessionRollbackGuard(SessionRollbackGuard&&) = delete;
    SessionRollbackGuard& operator=(SessionRollbackGuard&&) = delete;

    ~SessionRollbackGuard() noexcept {
      if (!active) return;
      // Keep the failed record published until SID-wide receiver cleanup is
      // complete. Otherwise a same-SID replacement could be created and have
      // its new generation's proof erased by this rollback.
      if (state->fence_dispatcher && record->fence_dispatch) {
        state->fence_dispatcher->CloseAndWait(record->fence_dispatch->durable);
        state->fence_dispatcher->CloseAndWait(record->fence_dispatch->ephemeral);
      }
      state->fence_receiver_registry->EraseSession(key);
      record->snapshot.reset();
      record->overlay.reset();
      record->durable_buffers.reset();
      record->metadata = {};
      std::unique_lock lock(state->session_mutex);
      auto it = state->sessions.find(key);
      if (it != state->sessions.end() && it->second == record) state->sessions.erase(it);
    }

    void Dismiss() noexcept { active = false; }
  };
  SessionRollbackGuard rollback{state.get(), key, record};

  std::shared_ptr<const StorageReader> snapshot;
  try {
    snapshot = state->engine->TakeSnapshot();
  } catch (...) {
    return Result<void>::Err(Status::Error, "base snapshot creation threw an exception");
  }
  if (!snapshot) {
    return Result<void>::Err(Status::Error, "base snapshot creation returned null");
  }

  record->snapshot = std::move(snapshot);
  record->overlay = std::make_shared<InMemoryEngine>();
  record->metadata = SessionMeta{NowIso8601()};

  if (options.durable_buffers) {
    if (!state->durable_buffer_engine_factory) {
      return Result<void>::Err(Status::InvalidArgument, "durable buffer engine factory is required",
                               std::make_error_code(std::errc::invalid_argument));
    }
    try {
      auto factory_result = state->durable_buffer_engine_factory(sid);
      if (!factory_result.IsOk()) return Result<void>::ErrFrom(factory_result);
      auto durable_engine = std::move(factory_result).Value();
      if (!durable_engine) {
        return Result<void>::Err(Status::Error, "durable buffer engine factory returned null");
      }
      record->durable_buffers = std::move(durable_engine);
    } catch (...) {
      return Result<void>::Err(Status::Error, "durable buffer engine factory threw an exception");
    }
  }

  bool committed = false;
  {
    std::unique_lock lock(state->session_mutex);
    auto it = state->sessions.find(key);
    if (it != state->sessions.end() && it->second == record) {
      committed = record->Activate();
    }
  }
  if (!committed) return Result<void>::Err(OperationInProgress());
  rollback.Dismiss();
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
  std::shared_ptr<SessionRecord> record;
  {
    std::unique_lock lock(state->session_mutex);
    auto it = state->sessions.find(key);
    if (it == state->sessions.end()) return Result<void>::Err(NoSuchSession());
    record = it->second;
    if (!record->BeginClose()) return Result<void>::Err(OperationInProgress());
  }

  if (state->fence_dispatcher) {
    state->fence_dispatcher->CloseAndWait(record->fence_dispatch->durable);
    state->fence_dispatcher->CloseAndWait(record->fence_dispatch->ephemeral);
  }
  {
    std::unique_lock lock(state->fence_test_mutex);
    if (state->fence_test_gate_close_after_dispatch) {
      state->fence_test_close_after_dispatch_reached = true;
      state->fence_test_condition.notify_all();
      state->fence_test_condition.wait(
          lock, [state] { return state->fence_test_release_close_after_dispatch; });
    }
  }
  record->WaitForAdmission();
  record->snapshot.reset();
  record->overlay.reset();
  record->durable_buffers.reset();
  record->metadata = {};
  record->generation_uuid = {};
  state->fence_receiver_registry->EraseSession(sid);
  {
    std::unique_lock lock(state->session_mutex);
    auto it = state->sessions.find(key);
    if (it != state->sessions.end() && it->second == record) state->sessions.erase(it);
  }
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
  for (const auto& [id, record] : state->sessions) {
    if (record->IsActive()) result.push_back(id);
  }
  return result;
}

StorageNode::SessionAccess StorageNode::AcquireSession(const std::shared_ptr<State>& state,
                                                       std::string_view sid) {
  SessionAccess access;
  std::shared_lock lock(state->session_mutex);
  if (auto it = state->sessions.find(sid); it != state->sessions.end()) {
    access.record = it->second;
    access.admission = access.record->TryAcquire();
  }
  return access;
}

void StorageNode::DispatchSample(const std::shared_ptr<State>& state,
                                 const TransportSample& sample) {
  try {
    {
      std::scoped_lock lock(state->fence_test_mutex);
      if (std::exchange(state->fence_test_throw_dispatch_once, false)) {
        throw std::bad_alloc();
      }
    }
    std::string sid;
    std::optional<BufferClass> buffer_class;
    bool marker = false;
    auto route = ParseFenceMarkerRoute(state->prefix, sample.key);
    const bool marker_route_valid = route.has_value();
    if (!route.has_value()) {
      route = fence_internal::RecoverMalformedFenceMarkerRoute(state->prefix, sample.key);
    }
    if (route && route->target == FenceMarkerTarget::Buffer) {
      sid = route->sid;
      buffer_class = route->buffer_class;
      marker = true;
    } else if (const auto parsed = ParseKey(state->prefix, sample.key);
               parsed && parsed->kind == KeyKind::Buffer &&
               !std::holds_alternative<FenceLaneAbsent>(sample.fence_lane)) {
      sid = parsed->sid;
      buffer_class = parsed->buffer_class;
    } else {
      OnSample(state, sample);
      return;
    }

    // Keep Session selection and dispatch admission in one read-side critical
    // section. CloseSession takes the unique side before BeginClose and final
    // erasure, so a selected marker is either admitted before Closing or its
    // InvalidArgument fallback completes before the record can disappear.
    std::shared_lock session_lock(state->session_mutex);
    std::shared_ptr<SessionRecord> record;
    const auto session = state->sessions.find(sid);
    if (session != state->sessions.end()) record = session->second;
    if (!record || !buffer_class.has_value()) {
      session_lock.unlock();
      if (marker) OnSample(state, sample);
      return;
    }
    if (!state->fence_dispatcher || !record->fence_dispatch) {
      session_lock.unlock();
      OnSample(state, sample);
      return;
    }
    auto& registration = *buffer_class == BufferClass::Durable ? record->fence_dispatch->durable
                                                               : record->fence_dispatch->ephemeral;
    const auto prepare = [&] {
      if (marker) return true;
      if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
        return state->fence_receiver_registry->AdmitBufferObservation(
            sid, record->generation_uuid, *buffer_class, lane->publisher_uuid, lane->sequence);
      }
      if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
        static_cast<void>(state->fence_receiver_registry->RecordMalformedBuffer(
            sid, record->generation_uuid, *buffer_class, *malformed));
      }
      return false;
    };
    const auto overflow = [&] {
      if (marker) return;
      if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
        static_cast<void>(state->fence_receiver_registry->RecordBufferOverflow(
            sid, record->generation_uuid, *buffer_class, lane->publisher_uuid, lane->sequence));
      } else if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
        static_cast<void>(state->fence_receiver_registry->RecordMalformedBuffer(
            sid, record->generation_uuid, *buffer_class, *malformed));
      }
    };
    auto admission = state->fence_dispatcher->Dispatch(registration, prepare, overflow);
    if (admission.outcome != fence_internal::FenceDispatchCoordinator::Outcome::Admitted) {
      // CloseSession closes class dispatch before erasing the Closing record. A
      // recoverable marker arriving in that interval still receives the existing
      // InvalidArgument lifecycle result instead of being silently dropped.
      if (marker &&
          admission.outcome == fence_internal::FenceDispatchCoordinator::Outcome::Closed) {
        {
          std::unique_lock lock(state->fence_test_mutex);
          if (state->fence_test_gate_closed_marker_fallback) {
            state->fence_test_closed_marker_fallback_reached = true;
            state->fence_test_condition.notify_all();
            state->fence_test_condition.wait(
                lock, [state] { return state->fence_test_release_closed_marker_fallback; });
          }
        }
        auto callback_lease = state->Enter();
        if (!callback_lease) return;
        try {
          // The closed-dispatch fallback performs no parameter or buffer application. Keep the
          // Session read lock until its immutable InvalidArgument result is retained so
          // CloseSession cannot erase the selected record first, but do not acquire the
          // application lane in the reverse of the normal lane-then-Session order.
          SubscriberDiagnostics diagnostics;
          ApplyBufferFenceMarker(state, *route, sample, diagnostics, marker_route_valid,
                                 Status::InvalidArgument);
          EmitDiagnostics(state->log_sink, diagnostics);
        } catch (...) {
          EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed);
        }
      }
      return;
    }
    session_lock.unlock();
    admission.entry.WaitTurn();
    OnSample(state, sample, !marker);
  } catch (...) {
    // No Transport implementation is required to contain user callback
    // exceptions, so the complete dispatch path is the boundary. Preserve
    // fail-closed ordering evidence for identifiable covered buffer data.
    std::shared_ptr<SessionRecord> failed_record;
    FenceDispatchCoordinator::Registration* registration = nullptr;
    bool retained = false;
    try {
      const auto parsed = ParseKey(state->prefix, sample.key);
      if (parsed && parsed->kind == KeyKind::Buffer && parsed->buffer_class.has_value()) {
        std::shared_lock lock(state->session_mutex);
        const auto session = state->sessions.find(parsed->sid);
        if (session != state->sessions.end() && session->second->fence_dispatch) {
          failed_record = session->second;
          registration = *parsed->buffer_class == BufferClass::Durable
                             ? &failed_record->fence_dispatch->durable
                             : &failed_record->fence_dispatch->ephemeral;
          if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
            static_cast<void>(state->fence_receiver_registry->RecordBufferOverflow(
                parsed->sid, failed_record->generation_uuid, *parsed->buffer_class,
                lane->publisher_uuid, lane->sequence));
            retained = true;
          } else if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
            static_cast<void>(state->fence_receiver_registry->RecordMalformedBuffer(
                parsed->sid, failed_record->generation_uuid, *parsed->buffer_class, *malformed));
            retained = true;
          }
        }
      }
    } catch (...) {
    }
    if (!retained && registration != nullptr && state->fence_dispatcher) {
      try {
        state->fence_dispatcher->CloseAndWait(*registration);
      } catch (...) {
      }
    }
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed);
  }
}

void StorageNode::OnSample(const std::shared_ptr<State>& state, const TransportSample& sample,
                           bool fence_preadmitted) {
  auto lease = state->Enter();
  if (!lease) return;
  FenceApplyProgress fence_progress;
  try {
    std::function<void()> subscriber_observer;
    {
      std::scoped_lock lock(state->test_observer_mutex);
      subscriber_observer = state->subscriber_entry_observer;
    }
    if (subscriber_observer) subscriber_observer();
    SubscriberDiagnostics diagnostics;
    auto marker = ParseFenceMarkerRoute(state->prefix, sample.key);
    bool marker_route_valid = marker.has_value();
    if (!marker.has_value()) {
      marker = fence_internal::RecoverMalformedFenceMarkerRoute(state->prefix, sample.key);
    }
    if (marker.has_value()) {
      if (marker->target == FenceMarkerTarget::Buffer) {
        if (state->application_owner.load() == std::this_thread::get_id()) {
          diagnostics.push_back({LogLevel::kError, kSubscriberReentry});
        } else {
          ApplicationLaneLock lane(state->subscriber_mutex, state->application_owner);
          ApplyBufferFenceMarker(state, *marker, sample, diagnostics, marker_route_valid);
        }
      }
    } else if (sample.key.starts_with(state->prefix + "/meta/fence/buffer/")) {
      // Without recoverable target/durability/through fields no truthful
      // Fence AckResult can be created; ignore without claiming the token.
    } else if (sample.key.starts_with(state->prefix + "/meta/fence/cache/")) {
      // Cache-target markers never create a StorageNode result.
    } else if (std::holds_alternative<AckAttachmentMalformed>(sample.ack)) {
      // ADR-0028: malformed acknowledgement metadata is rejected before application.
      diagnostics.push_back({LogLevel::kWarning, kMalformedAckAttachment});
    } else if (state->application_owner.load() == std::this_thread::get_id()) {
      // Same-thread reentry from inside an engine call: the parameter lane is
      // already held by this callback. ADR-0028 reports reentrant admission as an
      // invariant failure (Status::Error) without applying the second operation.
      // Independent concurrent deliveries never take this path; they wait below.
      if (const auto* token = std::get_if<AckToken>(&sample.ack)) {
        RejectReentrantAcknowledgedSample(state, *token, sample, diagnostics);
      } else {
        diagnostics.push_back({LogLevel::kError, kSubscriberReentry});
      }
    } else {
      // The lane lock serializes every parameter write, so an ordinary write can
      // never become visible between batch entries and at most one acknowledged
      // token is Processing at a time. Diagnostics are emitted after release.
      ApplicationLaneLock lane(state->subscriber_mutex, state->application_owner);
      if (const auto* token = std::get_if<AckToken>(&sample.ack)) {
        ApplyAcknowledgedSample(state, *token, sample, diagnostics);
      } else {
        ApplyParameterSample(state, sample, diagnostics, /*progress=*/nullptr, fence_preadmitted,
                             fence_preadmitted ? &fence_progress : nullptr);
      }
    }
    EmitDiagnostics(state->log_sink, diagnostics);
  } catch (...) {
    if (fence_preadmitted && !fence_progress.completion_retained) {
      try {
        const auto parsed = ParseKey(state->prefix, sample.key);
        if (parsed && parsed->kind == KeyKind::Buffer && parsed->buffer_class.has_value()) {
          std::shared_lock lock(state->session_mutex);
          const auto session = state->sessions.find(parsed->sid);
          const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane);
          if (session != state->sessions.end() && lane != nullptr) {
            state->fence_receiver_registry->CompleteBufferObservation(
                parsed->sid, session->second->generation_uuid, *parsed->buffer_class,
                lane->publisher_uuid, lane->sequence,
                fence_progress.application_started ? Status::OutcomeUnknown : Status::Error);
          }
        }
      } catch (...) {
      }
    }
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed);
  }
}

void StorageNode::ApplyBufferFenceMarker(const std::shared_ptr<State>& state,
                                         const FenceMarkerRoute& route,
                                         const TransportSample& sample,
                                         std::vector<SubscriberDiagnostic>& diagnostics,
                                         bool route_valid,
                                         std::optional<Status> forced_lifecycle_status) {
  const auto* token = std::get_if<AckToken>(&sample.ack);
  if (token == nullptr) return;

  const AckFingerprint fingerprint = ComputeAckFingerprint(AckOperationKind::Fence, sample.key,
                                                           sample.encoding.id, sample.payload);
  AckResultV1 result{AckOperationKind::Fence, Status::Error,          route.durability,     0,
                     kAckNoFailedIndex,       route.through_sequence, kAckNoFailedSequence, ""};
  const bool marker_valid = route_valid && sample.kind == TransportSample::Kind::Put &&
                            sample.encoding.id == Encoding::kSitosV1Fence &&
                            DecodeFenceMarker(sample.payload).IsOk();

  std::shared_ptr<SessionRecord> record;
  std::optional<SessionRecord::AdmissionLease> admission;
  if (marker_valid && forced_lifecycle_status.has_value()) {
    result.status = *forced_lifecycle_status;
  } else if (marker_valid) {
    std::shared_lock lock(state->session_mutex);
    const auto it = state->sessions.find(route.sid);
    if (it == state->sessions.end()) {
      result.status = Status::NotFound;
    } else {
      record = it->second;
      admission = record->TryAcquire();
      if (!admission.has_value()) {
        result.status = Status::InvalidArgument;
      } else if (record->generation_uuid != route.receiver_generation) {
        return;  // stale Session generation is rejected before token claim
      }
    }
  }

  const auto outcome = state->ack_registry->ClaimIndependent(*token, fingerprint);
  switch (outcome) {
    case AckRegistry::ClaimOutcome::DuplicateProcessing:
    case AckRegistry::ClaimOutcome::DuplicateCompleted:
      return;
    case AckRegistry::ClaimOutcome::Collision:
      diagnostics.push_back({LogLevel::kWarning, kAckTokenCollision});
      return;
    case AckRegistry::ClaimOutcome::LaneBusy:
      // Independent Fence markers never contend on a shared processing lane.
      diagnostics.push_back({LogLevel::kError, kAckLaneReentry});
      return;
    case AckRegistry::ClaimOutcome::Admitted:
      break;
  }
  FenceAckCompletionGuard completion(*state->ack_registry, *token, result);
  bool throw_after_claim = false;
  {
    std::unique_lock lock(state->fence_test_mutex);
    if (state->fence_test_last_claim_token == *token) {
      ++state->fence_test_last_claim_count;
    } else {
      state->fence_test_last_claim_token = *token;
      state->fence_test_last_claim_count = 1;
    }
    if (state->fence_test_gate_after_claim) {
      state->fence_test_claimed = true;
      state->fence_test_claimed_token = *token;
      state->fence_test_condition.notify_all();
      state->fence_test_condition.wait(lock, [state] { return state->fence_test_release_claim; });
    }
    throw_after_claim = std::exchange(state->fence_test_throw_after_claim_once, false);
  }

  if (!state->IsAccepting()) {
    result.status = Status::Error;
  } else if (record && admission.has_value() && marker_valid) {
    result.status = Status::Ok;
    // Inject at the latest pre-evaluation point so the exception-safety test
    // proves that a mutable optimistic result cannot escape through the
    // completion guard.
    if (throw_after_claim) throw std::runtime_error("injected Fence post-claim failure");
    if (!route.buffer_class.has_value()) {
      result.status = Status::InvalidArgument;
    } else {
      const bool buffer_enabled = *route.buffer_class == BufferClass::Durable
                                      ? record->options.durable_buffers
                                      : record->options.ephemeral_buffers;
      result = state->fence_receiver_registry->EvaluateBuffer(
          route.sid, route.receiver_generation, *route.buffer_class, route.publisher_uuid,
          route.durability, route.through_sequence);
      if (result.status == Status::Ok && !buffer_enabled) {
        result.status = Status::InvalidArgument;
      }
      if (result.status == Status::Ok && route.durability == AckDurability::Synced) {
        std::function<Result<void>(StorageEngine&)> barrier;
        {
          std::scoped_lock lock(state->fence_test_mutex);
          barrier = state->fence_test_durability_barrier;
        }
        if (!barrier || !record->durable_buffers) {
          result.status = Status::InvalidArgument;
        } else if (route.through_sequence > 0) {
          {
            std::scoped_lock lock(state->fence_test_mutex);
            ++state->fence_test_barrier_calls;
          }
          try {
            const auto synchronized = barrier(*record->durable_buffers);
            if (!synchronized.IsOk()) {
              result.status = synchronized.StatusCode();
              if (!ValidateAckResult(result).IsOk()) result.status = Status::Error;
            }
          } catch (...) {
            result.status = Status::OutcomeUnknown;
          }
        }
      }
    }
  } else if (record && admission.has_value()) {
    result.status = Status::Error;
  }

  {
    std::scoped_lock lock(state->fence_test_mutex);
    state->fence_test_last_completed_result = result;
  }
  completion.Finish(std::move(result));
}

AckResultV1 StorageNode::ApplyParameterSample(const std::shared_ptr<State>& state,
                                              const TransportSample& sample,
                                              std::vector<SubscriberDiagnostic>& diagnostics,
                                              AckApplyProgress* progress, bool fence_preadmitted,
                                              FenceApplyProgress* fence_progress) {
  const bool acknowledged = progress != nullptr;
  const auto parsed = ParseKey(state->prefix, sample.key);
  if (!parsed) {
    diagnostics.push_back({LogLevel::kWarning, kUnsupportedSubscriberKey});
    return PutFailure(Status::InvalidKey);
  }
  if (parsed->is_batch) {
    if (acknowledged) progress->kind = AckOperationKind::Batch;
    // ParseKey only marks Base and Session paths as batch paths.
    if (!IsBatchPut(sample)) {
      diagnostics.push_back({LogLevel::kWarning, kInvalidBatchOperation});
      return MakeAckResult(AckOperationKind::Batch, Status::InvalidArgument, 0, kAckNoFailedIndex);
    }
    if (parsed->kind == KeyKind::Base) {
      return ApplyBatch(diagnostics, *state->engine, sample.payload, progress);
    }
    auto access = AcquireSession(state, parsed->sid);
    if (!access.record || !access.admission.has_value()) {
      diagnostics.push_back({LogLevel::kWarning, kUnknownSession});
      return MakeAckResult(AckOperationKind::Batch, Status::NotFound, 0, kAckNoFailedIndex);
    }
    return ApplyBatch(diagnostics, *access.record->overlay, sample.payload, progress);
  }
  if (acknowledged && sample.kind == TransportSample::Kind::Delete) {
    // Delete remains acknowledgement-free in v1 (ADR-0028).
    diagnostics.push_back({LogLevel::kWarning, kAckUnsupportedOperation});
    return PutFailure(Status::InvalidArgument);
  }
  if (parsed->kind == KeyKind::Buffer) {
    if (acknowledged) {
      diagnostics.push_back({LogLevel::kWarning, kAckUnsupportedOperation});
      return PutFailure(Status::InvalidArgument);
    }
    auto access = AcquireSession(state, parsed->sid);
    if (!access.record || !access.admission.has_value()) {
      diagnostics.emplace_back(LogLevel::kWarning, kUnknownSession);
      if (fence_preadmitted && access.record && parsed->buffer_class.has_value()) {
        if (const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane)) {
          state->fence_receiver_registry->CompleteBufferObservation(
              parsed->sid, access.record->generation_uuid, *parsed->buffer_class,
              lane->publisher_uuid, lane->sequence, Status::InvalidArgument);
        }
      }
      return PutFailure(Status::InvalidArgument);
    }
    if (!parsed->buffer_class.has_value()) {
      diagnostics.emplace_back(LogLevel::kWarning, kBufferUnsupported);
      return PutFailure(Status::InvalidArgument);
    }
    if (const auto* malformed = std::get_if<FenceLaneMalformed>(&sample.fence_lane)) {
      static_cast<void>(state->fence_receiver_registry->RecordMalformedBuffer(
          parsed->sid, access.record->generation_uuid, *parsed->buffer_class, *malformed));
      return PutFailure(Status::InvalidArgument);
    }
    const auto* lane = std::get_if<FenceLaneMetadata>(&sample.fence_lane);
    if (lane != nullptr && !fence_preadmitted &&
        !state->fence_receiver_registry->AdmitBufferObservation(
            parsed->sid, access.record->generation_uuid, *parsed->buffer_class,
            lane->publisher_uuid, lane->sequence)) {
      return PutFailure(Status::InvalidArgument);
    }

    std::optional<Status> fence_failure;
    bool applied = false;
    if (sample.kind == TransportSample::Kind::Delete) {
      diagnostics.emplace_back(LogLevel::kWarning, kBufferUnsupported);
      fence_failure = Status::InvalidArgument;
    } else if (!IsBufferBytes(sample)) {
      diagnostics.emplace_back(LogLevel::kWarning, kBufferEncodingRejected);
      fence_failure = Status::InvalidArgument;
    } else if (*parsed->buffer_class == BufferClass::Ephemeral) {
      if (!access.record->options.ephemeral_buffers) {
        diagnostics.emplace_back(LogLevel::kWarning, kBufferCapabilityDisabled);
        fence_failure = Status::InvalidArgument;
      } else {
        applied = true;
        if (fence_progress != nullptr) fence_progress->application_started = true;
      }
    } else if (!access.record->options.durable_buffers || !access.record->durable_buffers) {
      diagnostics.emplace_back(LogLevel::kWarning, kBufferCapabilityDisabled);
      fence_failure = Status::InvalidArgument;
    } else {
      switch (ApplyDurableBufferWrite(*access.record->durable_buffers, *parsed, sample)) {
        case BufferWriteOutcome::Conflict:
          diagnostics.emplace_back(LogLevel::kWarning, kBufferPutConflict);
          fence_failure = Status::InvalidArgument;
          break;
        case BufferWriteOutcome::ReadFailed:
          diagnostics.emplace_back(LogLevel::kError, kBufferReadBeforePutFailed);
          fence_failure = Status::Error;
          break;
        case BufferWriteOutcome::WriteFailed:
          if (fence_progress != nullptr) fence_progress->application_started = true;
          diagnostics.emplace_back(LogLevel::kError, kBufferPutFailed);
          fence_failure = Status::OutcomeUnknown;
          break;
        case BufferWriteOutcome::Stored:
          applied = true;
          if (fence_progress != nullptr) fence_progress->application_started = true;
          break;
      }
    }
    if (lane != nullptr) {
      state->fence_receiver_registry->CompleteBufferObservation(
          parsed->sid, access.record->generation_uuid, *parsed->buffer_class, lane->publisher_uuid,
          lane->sequence, fence_failure);
      if (fence_progress != nullptr) fence_progress->completion_retained = true;
    }
    if (applied) {
      std::scoped_lock lock(state->fence_test_mutex);
      ++state->buffer_application_count;
    }
    return PutFailure(Status::InvalidArgument);  // unused by the acknowledgement-free path
  }
  switch (parsed->kind) {
    case KeyKind::Base:
      return ApplyWrite(diagnostics, *state->engine, parsed->relative_key, sample, progress);
    case KeyKind::Session: {
      auto access = AcquireSession(state, parsed->sid);
      if (!access.record || !access.admission.has_value()) {
        diagnostics.push_back({LogLevel::kWarning, kUnknownSession});
        return PutFailure(Status::NotFound);
      }
      return ApplyWrite(diagnostics, *access.record->overlay, parsed->relative_key, sample,
                        progress);
    }
    case KeyKind::Snapshot:
      diagnostics.push_back({LogLevel::kWarning, kReadOnlySnapshotKey});
      return PutFailure(Status::ReadOnly);
    default:  // MetaSession, MetaAck: not writable via subscriber.
      diagnostics.push_back({LogLevel::kWarning, kUnsupportedSubscriberKey});
      return PutFailure(Status::InvalidKey);
  }
}

void StorageNode::ApplyAcknowledgedSample(const std::shared_ptr<State>& state,
                                          const AckToken& token, const TransportSample& sample,
                                          std::vector<SubscriberDiagnostic>& diagnostics) {
  AckRegistry& registry = *state->ack_registry;
  const auto parsed = ParseKey(state->prefix, sample.key);
  const AckOperationKind kind =
      parsed && parsed->is_batch ? AckOperationKind::Batch : AckOperationKind::Put;
  const AckFingerprint fingerprint =
      ComputeAckFingerprint(kind, sample.key, sample.encoding.id, sample.payload);

  // The caller holds the parameter lane, so the claim is the token linearization
  // point for every tokenized sample on that lane and the registry mutex is released
  // before engine application. A busy-lane rejection (cannot happen while the lane is
  // held, kept as a defensive invariant) is retained in the same critical section.
  switch (registry.ClaimOrReject(token, fingerprint, AckRegistry::kParameterLane,
                                 LaneBusyResult(kind))) {
    case AckRegistry::ClaimOutcome::DuplicateProcessing:
    case AckRegistry::ClaimOutcome::DuplicateCompleted:
      return;  // never repeat apply; the retained result answers queries
    case AckRegistry::ClaimOutcome::Collision:
      diagnostics.push_back({LogLevel::kWarning, kAckTokenCollision});
      return;
    case AckRegistry::ClaimOutcome::LaneBusy:
      diagnostics.push_back({LogLevel::kError, kAckLaneReentry});
      return;
    case AckRegistry::ClaimOutcome::Admitted:
      break;
  }

  AckApplyProgress progress;
  progress.kind = kind;
  AckCompletionGuard guard(registry, token, progress);
  AckResultV1 result = ApplyParameterSample(state, sample, diagnostics, &progress);
  guard.Finish(std::move(result));
}

void StorageNode::RejectReentrantAcknowledgedSample(
    const std::shared_ptr<State>& state, const AckToken& token, const TransportSample& sample,
    std::vector<SubscriberDiagnostic>& diagnostics) {
  const auto parsed = ParseKey(state->prefix, sample.key);
  const AckOperationKind kind =
      parsed && parsed->is_batch ? AckOperationKind::Batch : AckOperationKind::Put;
  const AckFingerprint fingerprint =
      ComputeAckFingerprint(kind, sample.key, sample.encoding.id, sample.payload);
  diagnostics.push_back({LogLevel::kError, kAckLaneReentry});
  // Retained outside the lane: later deliveries of the reentrant token are duplicates
  // of this Error result and are never applied.
  state->ack_registry->RecordRejected(token, fingerprint, LaneBusyResult(kind));
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
    } else if (head == "buffers") {
      ReplyBufferQuery(state, query);
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

  std::optional<SessionRecord::AdmissionLease> admission;
  std::shared_ptr<SessionRecord> record;
  std::shared_ptr<const StorageReader> reader;
  {
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(sid);
    if (it != state->sessions.end()) {
      record = it->second;
      admission = record->TryAcquire();
      if (admission.has_value()) {
        reader = scope == "snap" ? record->snapshot : record->overlay;
      }
    }
  }
  if (!record || !admission.has_value() || !reader) return;

  const std::string scope_path = std::string(scope) + "/" + std::string(sid);
  ReplyFromReader(*reader, *selector, state->prefix, scope_path, query);
}

void StorageNode::ReplyBufferQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  auto rest = StripPrefix(state->prefix, query.keyexpr);
  if (!rest || !rest->starts_with("buffers/")) return;
  auto sid_split = SplitFirst(rest->substr(8));
  if (!sid_split) return;
  const auto& [sid, class_and_selector] = *sid_split;
  auto class_split = SplitFirst(class_and_selector);
  if (!class_split || !IsValidSessionId(sid)) return;
  const auto& [class_name, selector_text] = *class_split;
  if (class_name != "durable") return;
  if (selector_text.empty()) return;
  std::string relative;
  bool list = false;
  if (selector_text == "**") {
    list = true;
  } else if (selector_text.ends_with("/**")) {
    auto prefix = selector_text.substr(0, selector_text.size() - 3);
    if (!IsValidPrefix(prefix)) return;
    relative = std::string(prefix) + "/";
    list = true;
  } else {
    if (!IsValidKey(selector_text)) return;
    relative = std::string(selector_text);
  }

  struct OwnedEntry {
    std::string key;
    std::vector<std::byte> value;
  };
  std::vector<OwnedEntry> entries;
  bool ok = false;
  bool collection_failed = false;
  {
    std::optional<SessionRecord::AdmissionLease> admission;
    std::shared_ptr<SessionRecord> record;
    {
      std::shared_lock lock(state->session_mutex);
      auto it = state->sessions.find(sid);
      if (it == state->sessions.end()) return;
      record = it->second;
      admission = record->TryAcquire();
    }
    if (!admission || !record->options.durable_buffers || !record->durable_buffers) return;
    try {
      auto sink = [&entries](std::string_view key, Bytes value) {
        entries.emplace_back(std::string(key), std::vector<std::byte>(value.begin(), value.end()));
        return true;
      };
      ok = list ? record->durable_buffers->List(relative, sink)
                : record->durable_buffers->Get(relative, sink);
    } catch (...) {
      collection_failed = true;
    }
  }
  if (collection_failed || (!ok && list)) {
    EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kBufferQueryFailed);
    return;
  }

  const Encoding encoding{"zenoh/bytes"};
  bool dispatch = true;
  for (const auto& entry : entries) {
    if (!dispatch) break;
    const auto key =
        MakeReplyKey(state->prefix, "buffers/" + std::string(sid) + "/durable", entry.key);
    try {
      auto result = query.Reply(key, entry.value, encoding);
      if (!result.IsOk()) {
        dispatch = false;
        EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kBufferQueryFailed);
      }
    } catch (...) {
      dispatch = false;
      EmitLog(state->log_sink, LogLevel::kError, kNodeComponent, kBufferQueryFailed);
    }
  }
}

void StorageNode::ReplyMetaQuery(const std::shared_ptr<State>& state, TransportQuery& query) {
  auto parsed = ParseKey(state->prefix, query.keyexpr);
  if (!parsed) return;
  if (parsed->kind == KeyKind::MetaAck) {
    // ADR-0028: only canonical sitos-generated UUIDv4 text names a result; absent,
    // Processing, evicted, and restart-lost tokens return zero replies.
    const auto token = ParseAckToken(parsed->uuid);
    if (!token) return;
    const auto result = state->ack_registry->Find(*token);
    if (!result) return;
    auto encoded = EncodeAckResult(*result);
    if (!encoded.IsOk()) return;
    query.Reply(query.keyexpr, encoded.Value(), Encoding{std::string(Encoding::kSitosV1Ack)});
    return;
  }
  if (parsed->kind != KeyKind::MetaSession) return;

  std::string json;
  std::optional<SessionRecord::AdmissionLease> admission;
  {
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(parsed->sid);
    if (it == state->sessions.end() || !it->second->IsActive()) {
      return;  // Unknown or non-active sid: 0 replies.
    }
    admission = it->second->TryAcquire();
    if (!admission.has_value()) return;
    json =
        std::format(R"({{"state":"active","created_at":"{}"}})", it->second->metadata.created_at);
  }
  const auto payload = ParamValue(json).Encode();
  query.Reply(query.keyexpr, payload, SitosEncoding());
}

namespace fence_test_access {

bool FenceTestAccess::SetSessionGeneration(StorageNode& node, std::string_view sid,
                                           const FenceUuid& generation) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->session_mutex);
  const auto it = state->sessions.find(sid);
  if (it == state->sessions.end()) return false;
  it->second->generation_uuid = generation;
  return true;
}

bool FenceTestAccess::BufferValueExists(StorageNode& node, std::string_view sid,
                                        std::string_view key) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::shared_ptr<StorageNode::SessionRecord> record;
  std::optional<StorageNode::SessionRecord::AdmissionLease> admission;
  {
    std::shared_lock lock(state->session_mutex);
    const auto it = state->sessions.find(sid);
    if (it == state->sessions.end()) return false;
    record = it->second;
    admission = record->TryAcquire();
  }
  if (!admission || !record->durable_buffers) return false;
  return record->durable_buffers->Get(key, [](std::string_view, Bytes) { return true; });
}

std::optional<AckResultV1> FenceTestAccess::FindAckResult(StorageNode& node,
                                                          const AckToken& token) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  return state ? state->ack_registry->Find(token) : std::nullopt;
}

std::size_t FenceTestAccess::FenceTokenClaims(StorageNode& node, const AckToken& token) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return 0;
  std::scoped_lock lock(state->fence_test_mutex);
  return state->fence_test_last_claim_token == token ? state->fence_test_last_claim_count : 0;
}

std::size_t FenceTestAccess::BufferApplicationCount(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return 0;
  std::scoped_lock lock(state->fence_test_mutex);
  return state->buffer_application_count;
}

std::size_t FenceTestAccess::AckRegistryEntries(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  return state ? state->ack_registry->Size() : 0;
}

std::size_t FenceTestAccess::AckRegistryProcessingEntries(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  return state ? state->ack_registry->ProcessingCount() : 0;
}

bool FenceTestAccess::ThrowStorageDispatchOnce(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_throw_dispatch_once = true;
  return true;
}

bool FenceTestAccess::ThrowFenceAfterTokenClaimOnce(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_throw_after_claim_once = true;
  return true;
}

bool FenceTestAccess::FailFenceCompletionRetentionOnce(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  state->ack_registry->FailNextForTesting(
      AckRegistry::FailurePointForTesting::BeforeCompletionRetention);
  return true;
}

std::size_t FenceTestAccess::FenceReceiverRegistryEntries(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  return state ? state->fence_receiver_registry->Size() : 0;
}

std::optional<FenceUuid> FenceTestAccess::VisibleSessionGeneration(StorageNode& node,
                                                                   std::string_view sid) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return std::nullopt;
  std::shared_lock lock(state->session_mutex);
  const auto it = state->sessions.find(sid);
  return it == state->sessions.end() ? std::nullopt
                                     : std::optional<FenceUuid>{it->second->generation_uuid};
}

bool FenceTestAccess::BeginClose(StorageNode& node, std::string_view sid) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->session_mutex);
  const auto it = state->sessions.find(sid);
  return it != state->sessions.end() && it->second->BeginClose();
}

bool FenceTestAccess::CloseSessionFenceDispatch(StorageNode& node, std::string_view sid) {
  std::shared_ptr<StorageNode::State> state;
  std::shared_ptr<StorageNode::SessionRecord> record;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state || !state->fence_dispatcher) return false;
  {
    std::shared_lock lock(state->session_mutex);
    const auto it = state->sessions.find(sid);
    if (it == state->sessions.end()) return false;
    record = it->second;
  }
  state->fence_dispatcher->CloseAndWait(record->fence_dispatch->durable);
  state->fence_dispatcher->CloseAndWait(record->fence_dispatch->ephemeral);
  return true;
}

bool FenceTestAccess::SetFenceDurabilityBarrier(
    StorageNode& node, std::function<Result<void>(StorageEngine&)> barrier) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_durability_barrier = std::move(barrier);
  return true;
}

std::size_t FenceTestAccess::FenceDurabilityBarrierCalls(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return 0;
  std::scoped_lock lock(state->fence_test_mutex);
  return state->fence_test_barrier_calls;
}

bool FenceTestAccess::CloseAndRecreateSession(StorageNode& node, std::string_view sid,
                                              SessionOptions options,
                                              const FenceUuid& repeated_generation) {
  if (!node.CloseSession(sid).IsOk() || !node.CreateSession(sid, options).IsOk()) return false;
  return SetSessionGeneration(node, sid, repeated_generation);
}

bool FenceTestAccess::GateSessionAfterFenceRegistration(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
    node.fence_test_retained_state_ = state;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_gate_session_registration = true;
  state->fence_test_session_registration_reached = false;
  state->fence_test_release_session_registration = false;
  return true;
}

bool FenceTestAccess::WaitForSessionFenceRegistration(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->fence_test_mutex);
  state->fence_test_condition.wait(
      lock, [state] { return state->fence_test_session_registration_reached; });
  return true;
}

bool FenceTestAccess::ReleaseSessionFenceRegistration(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_release_session_registration = true;
  state->fence_test_condition.notify_all();
  return true;
}

bool FenceTestAccess::GateCloseAfterFenceDispatch(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_gate_close_after_dispatch = true;
  state->fence_test_close_after_dispatch_reached = false;
  state->fence_test_release_close_after_dispatch = false;
  return true;
}

bool FenceTestAccess::WaitForCloseAfterFenceDispatch(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->fence_test_mutex);
  state->fence_test_condition.wait(
      lock, [state] { return state->fence_test_close_after_dispatch_reached; });
  return true;
}

bool FenceTestAccess::ReleaseCloseAfterFenceDispatch(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_release_close_after_dispatch = true;
  state->fence_test_condition.notify_all();
  return true;
}

bool FenceTestAccess::GateClosedMarkerFallback(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_gate_closed_marker_fallback = true;
  state->fence_test_closed_marker_fallback_reached = false;
  state->fence_test_release_closed_marker_fallback = false;
  return true;
}

bool FenceTestAccess::WaitForClosedMarkerFallback(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->fence_test_mutex);
  state->fence_test_condition.wait(
      lock, [state] { return state->fence_test_closed_marker_fallback_reached; });
  return true;
}

bool FenceTestAccess::ReleaseClosedMarkerFallback(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_release_closed_marker_fallback = true;
  state->fence_test_condition.notify_all();
  return true;
}

bool FenceTestAccess::GateFenceAfterTokenClaim(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
    node.fence_test_retained_state_ = state;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_gate_after_claim = true;
  state->fence_test_claimed = false;
  state->fence_test_release_claim = false;
  return true;
}

bool FenceTestAccess::WaitForFenceTokenClaim(StorageNode& node, const AckToken& token) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->fence_test_mutex);
  state->fence_test_condition.wait(lock, [state, &token] {
    return state->fence_test_claimed && state->fence_test_claimed_token == token;
  });
  return true;
}

bool FenceTestAccess::ReleaseFenceAfterTokenClaim(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->fence_test_mutex);
  state->fence_test_release_claim = true;
  state->fence_test_condition.notify_all();
  return true;
}

bool FenceTestAccess::WaitUntilStorageNodeAdmissionClosed(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::unique_lock lock(state->gate_mutex);
  state->gate_cv.wait(lock, [state] { return !state->accepting; });
  return true;
}

std::optional<AckResultV1> FenceTestAccess::LastCompletedFenceResult(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return std::nullopt;
  std::scoped_lock lock(state->fence_test_mutex);
  return state->fence_test_last_completed_result;
}

bool FenceTestAccess::StorageNodeCallbacksQuiesced(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return true;
  std::scoped_lock lock(state->gate_mutex);
  return state->in_flight == 0;
}

bool FenceTestAccess::LateNodeCallbackCanAccessState(StorageNode& node) {
  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_ ? node.state_ : node.fence_test_retained_state_;
  }
  if (!state) return false;
  std::scoped_lock lock(state->gate_mutex);
  return state->accepting;
}

}  // namespace fence_test_access

}  // namespace sitos
