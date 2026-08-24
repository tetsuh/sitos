// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for the base storage scope.

#ifndef SITOS_STORAGE_NODE_HPP
#define SITOS_STORAGE_NODE_HPP

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/logging.hpp"
#include "sitos/session.hpp"
#include "sitos/storage_engine.hpp"
#include "sitos/transport.hpp"

namespace sitos {

/// Creates the one durable engine owned by a durable-enabled Session.
/// CreateSession calls may run concurrently; factories with mutable shared state must synchronize
/// that state themselves. The factory must not synchronously wait for same-node lifecycle
/// quiescence operations.
using DurableBufferEngineFactory =
    std::function<Result<std::unique_ptr<StorageEngine>>(std::string_view sid)>;

/// The supported query shape for the StorageNode base scope.
struct StorageQuery {
  /// True for a terminal star-star selector, false for an exact key.
  bool is_list = false;
  /// Exact relative key, or the relative List prefix including its trailing `/`.
  std::string relative_key;
};

/// Parses an exact base key or a terminal base List selector.
///
/// The returned exact key is relative to the base scope. For a List selector,
/// the returned prefix ends at a chunk boundary and includes its trailing `/`.
std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix, std::string_view keyexpr);

struct StorageNodeConfig {
  std::string prefix = "sitos";
  /// Diagnostic destination; nullptr explicitly disables logging.
  std::shared_ptr<LogSink> log_sink = DefaultLogSink();
  DurableBufferEngineFactory durable_buffer_engine_factory = {};
};

class SessionView;
class AckRegistry;
namespace fence_internal {
class FenceDispatchCoordinator;
class FenceReceiverRegistry;
struct FenceMarkerRoute;
struct FenceSessionDispatch;
}  // namespace fence_internal
struct SubscriberDiagnostic;
struct AckApplyProgress;
struct FenceApplyProgress;
namespace storage_node_test_access {
class StorageNodeTestAccess;
}
namespace fence_test_access {
class FenceTestAccess;
}

/// Serves base-scope Get/List queries and base writes through Transport declarations.
class StorageNode {
 public:
  using Config = StorageNodeConfig;

  StorageNode() = default;
  explicit StorageNode(Transport& transport) : transport_(&transport) {}
  ~StorageNode();

  StorageNode(const StorageNode&) = delete;
  StorageNode& operator=(const StorageNode&) = delete;
  StorageNode(StorageNode&&) = delete;
  StorageNode& operator=(StorageNode&&) = delete;

  /// Starts the node using the transport supplied to the constructor.
  Result<void> Start(std::shared_ptr<StorageEngine> engine, Config config);

  /// Starts the node using an externally owned transport.
  Result<void> Start(std::shared_ptr<StorageEngine> engine, Transport& transport, Config config);

  /// Opens a session: takes an engine snapshot for snap/<sid>/** reads and
  /// creates an empty overlay for session/<sid>/** reads and writes. Fails with
  /// invalid_argument for a malformed sid, an absent node State, or a captured State whose
  /// lifecycle gate is closed; the latter two return an empty message. Returns file_exists if
  /// the session already exists. [F10]
  Result<void> CreateSession(std::string_view sid);

  /// Opens a session with explicit durable and ephemeral buffer capabilities. The same
  /// stopped/captured-closed-gate InvalidArgument contract applies. Durable creation requires the
  /// configured factory and reports factory/setup failures.
  Result<void> CreateSession(std::string_view sid, SessionOptions options);

  /// Closes a session: releases its snapshot, overlay, and durable engine, then removes its
  /// metadata, so subsequent snap/session/meta gets reply nothing. Fails with
  /// invalid_argument for a stopped node and no_such_file_or_directory for an
  /// unknown sid. [F10]
  Result<void> CloseSession(std::string_view sid);

  /// Returns the ids of all active sessions in unspecified order. Empty when the
  /// node is stopped.
  std::vector<std::string> ActiveSessions() const;

  /// Undeclares the queryable and subscriber. Safe to call repeatedly and concurrently.
  /// Callbacks already in flight are completed before this returns. Calling lifecycle methods
  /// or destroying this object from one of its callbacks is not supported.
  void Stop() noexcept;

  /// Thread-safe lifecycle observation. The caller-owned Transport must outlive this node and
  /// all concurrent calls. StorageNode is intentionally non-copyable and non-movable.
  bool IsStarted() const noexcept;

 private:
  friend class SessionView;
  friend class storage_node_test_access::StorageNodeTestAccess;
  friend class fence_test_access::FenceTestAccess;

  struct SessionRecord {
    enum class Phase { Creating, Active, Closing };

    class AdmissionLease {
     public:
      explicit AdmissionLease(SessionRecord* record) : record_(record) {}
      ~AdmissionLease() {
        if (record_ != nullptr) record_->LeaveAdmission();
      }
      AdmissionLease(const AdmissionLease&) = delete;
      AdmissionLease& operator=(const AdmissionLease&) = delete;
      AdmissionLease(AdmissionLease&& other) noexcept : record_(other.record_) {
        other.record_ = nullptr;
      }
      AdmissionLease& operator=(AdmissionLease&& other) noexcept {
        if (this != &other) {
          if (record_ != nullptr) record_->LeaveAdmission();
          record_ = other.record_;
          other.record_ = nullptr;
        }
        return *this;
      }

     private:
      SessionRecord* record_;
    };

    std::optional<AdmissionLease> TryAcquire() noexcept {
      std::scoped_lock lock(admission_mutex);
      if (phase != Phase::Active || !accepting) return std::nullopt;
      ++admitted;
      return AdmissionLease(this);
    }

    bool Activate() noexcept {
      std::scoped_lock lock(admission_mutex);
      if (phase != Phase::Creating) return false;
      phase = Phase::Active;
      accepting = true;
      admission_cv.notify_all();
      return true;
    }

    bool IsActive() noexcept {
      std::scoped_lock lock(admission_mutex);
      return phase == Phase::Active;
    }

    bool BeginClose() noexcept {
      std::scoped_lock lock(admission_mutex);
      if (phase != Phase::Active) return false;
      phase = Phase::Closing;
      accepting = false;
      admission_cv.notify_all();
      return true;
    }

    void WaitForClosing() noexcept {
      std::unique_lock lock(admission_mutex);
      admission_cv.wait(lock, [this] { return phase == Phase::Closing; });
    }

    void WaitForAdmission() noexcept {
      std::unique_lock lock(admission_mutex);
      admission_cv.wait(lock, [this] { return admitted == 0; });
    }

    void LeaveAdmission() noexcept {
      std::scoped_lock lock(admission_mutex);
      assert(admitted > 0);
      if (admitted == 0) return;
      --admitted;
      if (admitted == 0) admission_cv.notify_all();
    }

    Phase phase = Phase::Creating;
    bool accepting = false;
    std::size_t admitted = 0;
    std::mutex admission_mutex;
    std::condition_variable admission_cv;
    std::shared_ptr<const StorageReader> snapshot;
    std::shared_ptr<StorageEngine> overlay;
    std::unique_ptr<StorageEngine> durable_buffers;
    SessionOptions options;
    SessionMeta metadata;
    FenceUuid generation_uuid{};
    std::shared_ptr<fence_internal::FenceSessionDispatch> fence_dispatch;
  };

  struct SessionKeyHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  struct State {
    State(std::shared_ptr<StorageEngine> storage, std::string key_prefix,
          std::shared_ptr<LogSink> diagnostics, DurableBufferEngineFactory durable_factory)
        : engine(std::move(storage)),
          prefix(std::move(key_prefix)),
          log_sink(std::move(diagnostics)),
          durable_buffer_engine_factory(std::move(durable_factory)) {}

    std::shared_ptr<StorageEngine> engine;
    std::string prefix;
    const std::shared_ptr<LogSink> log_sink;
    DurableBufferEngineFactory durable_buffer_engine_factory;
    // ADR-0028 node-wide token registry and completion ring; owned by this live State.
    // Created by Start, cleared by Stop; never shared across State generations.
    std::shared_ptr<AckRegistry> ack_registry;
    std::shared_ptr<fence_internal::FenceDispatchCoordinator> fence_dispatcher;
    std::shared_ptr<fence_internal::FenceReceiverRegistry> fence_receiver_registry;
    mutable std::mutex fence_test_mutex;
    std::optional<AckToken> fence_test_last_claim_token;
    std::size_t fence_test_last_claim_count = 0;
    std::size_t buffer_application_count = 0;
    std::condition_variable fence_test_condition;
    bool fence_test_gate_after_claim = false;
    bool fence_test_throw_dispatch_once = false;
    bool fence_test_throw_after_claim_once = false;
    bool fence_test_claimed = false;
    bool fence_test_release_claim = false;
    AckToken fence_test_claimed_token{};
    std::optional<AckResultV1> fence_test_last_completed_result;
    std::function<Result<void>(StorageEngine&)> fence_test_durability_barrier;
    std::size_t fence_test_barrier_calls = 0;
    bool fence_test_gate_session_registration = false;
    bool fence_test_session_registration_reached = false;
    bool fence_test_release_session_registration = false;
    bool fence_test_gate_close_after_dispatch = false;
    bool fence_test_close_after_dispatch_reached = false;
    bool fence_test_release_close_after_dispatch = false;
    bool fence_test_gate_closed_marker_fallback = false;
    bool fence_test_closed_marker_fallback_reached = false;
    bool fence_test_release_closed_marker_fallback = false;

    class CallbackLease {
     public:
      explicit CallbackLease(State* state) : state_(state) {}
      ~CallbackLease() {
        if (state_ != nullptr) state_->Leave();
      }
      CallbackLease(const CallbackLease&) = delete;
      CallbackLease& operator=(const CallbackLease&) = delete;
      CallbackLease(CallbackLease&& other) noexcept : state_(other.state_) {
        other.state_ = nullptr;
      }
      CallbackLease& operator=(CallbackLease&& other) noexcept {
        if (this != &other) {
          if (state_ != nullptr) state_->Leave();
          state_ = other.state_;
          other.state_ = nullptr;
        }
        return *this;
      }

     private:
      State* state_;
    };

    std::optional<CallbackLease> Enter() noexcept {
      std::scoped_lock lock(gate_mutex);
      if (!accepting) return std::nullopt;
      ++in_flight;
      return CallbackLease(this);
    }

    void Activate() noexcept {
      std::scoped_lock lock(gate_mutex);
      accepting = true;
    }

    void DeactivateAndWait() noexcept {
      std::unique_lock lock(gate_mutex);
      accepting = false;
      gate_cv.notify_all();
      gate_cv.wait(lock, [this] { return in_flight == 0; });
    }

    bool IsAccepting() noexcept {
      std::scoped_lock lock(gate_mutex);
      return accepting;
    }

    void Leave() noexcept {
      std::scoped_lock lock(gate_mutex);
      assert(in_flight > 0);
      if (in_flight == 0) return;
      --in_flight;
      if (in_flight == 0) gate_cv.notify_all();
    }

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::size_t in_flight = 0;
    bool accepting = false;

    // Subscriber callbacks enter the callback gate, then take
    // subscriber_mutex before optionally looking up a session under
    // session_mutex. This prevents ordinary writes from interleaving a batch;
    // session locks are released before engine writes.
    std::mutex subscriber_mutex;
    // Thread currently holding subscriber_mutex (the ADR-0028 parameter lane), or a
    // default-constructed id. A callback re-entering OnSample from inside an engine
    // call on the same thread is rejected instead of deadlocking on the lane.
    std::atomic<std::thread::id> application_owner{};

    // Test-only observers are unset in production use. They are copied under
    // test_observer_mutex and invoked without holding State locks.
    mutable std::mutex test_observer_mutex;
    std::function<void()> subscriber_entry_observer;
    std::function<void()> create_session_entry_observer;

    // Session records are the sole internal ownership and membership source.
    // Guarded by session_mutex. Callbacks and session operations alike enter
    // the callback gate before locking session_mutex, so the
    // gate -> subscriber_mutex -> session_mutex -> admission ordering never
    // cycles.
    std::shared_mutex session_mutex;
    std::unordered_map<std::string, std::shared_ptr<SessionRecord>, SessionKeyHash, std::equal_to<>>
        sessions;
  };

  struct SessionAccess {
    std::optional<SessionRecord::AdmissionLease> admission;
    std::shared_ptr<SessionRecord> record;
  };

  static SessionAccess AcquireSession(const std::shared_ptr<State>& state, std::string_view sid);
  static void OnQuery(const std::shared_ptr<State>& state, TransportQuery& query);
  static void DispatchSample(const std::shared_ptr<State>& state, const TransportSample& sample);
  static void OnSample(const std::shared_ptr<State>& state, const TransportSample& sample,
                       bool fence_preadmitted = false);
  static void ApplyBufferFenceMarker(const std::shared_ptr<State>& state,
                                     const fence_internal::FenceMarkerRoute& route,
                                     const TransportSample& sample,
                                     std::vector<SubscriberDiagnostic>& diagnostics,
                                     bool route_valid = true,
                                     std::optional<Status> forced_lifecycle_status = std::nullopt);
  // Shared parameter-write application for acknowledged and acknowledgement-free samples
  // (ADR-0028 stop-first batches). Returns the typed outcome; ack-free callers ignore it.
  static AckResultV1 ApplyParameterSample(const std::shared_ptr<State>& state,
                                          const TransportSample& sample,
                                          std::vector<SubscriberDiagnostic>& diagnostics,
                                          AckApplyProgress* progress,
                                          bool fence_preadmitted = false,
                                          FenceApplyProgress* fence_progress = nullptr);
  // Claims the token, applies through ApplyParameterSample, and publishes exactly one result.
  static void ApplyAcknowledgedSample(const std::shared_ptr<State>& state, const AckToken& token,
                                      const TransportSample& sample,
                                      std::vector<SubscriberDiagnostic>& diagnostics);
  // Same-thread reentry on the held lane: retains Status::Error without application.
  static void RejectReentrantAcknowledgedSample(const std::shared_ptr<State>& state,
                                                const AckToken& token,
                                                const TransportSample& sample,
                                                std::vector<SubscriberDiagnostic>& diagnostics);
  static Result<void> CreateSession(const std::shared_ptr<State>& state, std::string_view sid,
                                    SessionOptions options);
  // Answers a get in the session or snap scope from the matching overlay or
  // snapshot; replies nothing for an unknown sid.
  static void ReplyScopedQuery(const std::shared_ptr<State>& state, std::string_view scope,
                               std::string_view tail, TransportQuery& query);
  // Answers a get on meta/session/<sid> with the session metadata JSON.
  static void ReplyMetaQuery(const std::shared_ptr<State>& state, TransportQuery& query);
  static void ReplyBufferQuery(const std::shared_ptr<State>& state, TransportQuery& query);

  mutable std::mutex lifecycle_mutex_;
  // Serializes declaration/undeclaration transactions. Callbacks never hold this lock.
  mutable std::mutex operation_mutex_;
  Transport* transport_ = nullptr;
  std::shared_ptr<State> state_;
  std::shared_ptr<State> fence_test_retained_state_;
  Queryable queryable_;
  Subscription subscriber_;
};

}  // namespace sitos

#endif  // SITOS_STORAGE_NODE_HPP
