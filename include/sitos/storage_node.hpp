// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for the base storage scope.

#ifndef SITOS_STORAGE_NODE_HPP
#define SITOS_STORAGE_NODE_HPP

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
#include <unordered_map>
#include <vector>

#include "sitos/logging.hpp"
#include "sitos/session.hpp"
#include "sitos/storage_engine.hpp"
#include "sitos/transport.hpp"

namespace sitos {

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
namespace storage_node_test_access {
class StorageNodeTestAccess;
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
  /// invalid_argument for a malformed sid or a stopped node, and file_exists if
  /// the session already exists. [F10]
  Result<void> CreateSession(std::string_view sid);

  /// Opens a session with explicit durable and ephemeral buffer capabilities.
  Result<void> CreateSession(std::string_view sid, SessionOptions options);

  /// Closes a session: releases its snapshot and overlay and removes its
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
  static void OnSample(const std::shared_ptr<State>& state, const TransportSample& sample);
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
  Queryable queryable_;
  Subscription subscriber_;
};

}  // namespace sitos

#endif  // SITOS_STORAGE_NODE_HPP
