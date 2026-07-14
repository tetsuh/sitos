// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for the base storage scope.

#ifndef SITOS_STORAGE_NODE_HPP
#define SITOS_STORAGE_NODE_HPP

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sitos/logging.hpp"
#include "sitos/session.hpp"
#include "sitos/storage_engine.hpp"
#include "sitos/transport.hpp"

namespace sitos {

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
std::optional<StorageQuery> ParseStorageQuery(std::string_view prefix,
                                               std::string_view keyexpr);

struct StorageNodeConfig {
  std::string prefix = "sitos";
  /// Diagnostic destination; nullptr explicitly disables logging.
  std::shared_ptr<LogSink> log_sink = DefaultLogSink();
};

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
  Result<void> Start(std::shared_ptr<StorageEngine> engine, Transport& transport,
                     Config config);

  /// Opens a session: takes an engine snapshot for snap/<sid>/** reads and
  /// creates an empty overlay for session/<sid>/** reads and writes. Fails with
  /// invalid_argument for a malformed sid or a stopped node, and file_exists if
  /// the session already exists. [F10]
  Result<void> CreateSession(std::string_view sid);

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
  struct State {
    State(std::shared_ptr<StorageEngine> storage, std::string key_prefix,
          std::shared_ptr<LogSink> diagnostics)
        : engine(std::move(storage)),
          prefix(std::move(key_prefix)),
          log_sink(std::move(diagnostics)) {}

    std::shared_ptr<StorageEngine> engine;
    std::string prefix;
    const std::shared_ptr<LogSink> log_sink;

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

    // Session tables. Guarded by session_mutex. Callbacks and session
    // operations alike enter the callback gate before locking session_mutex,
    // so the gate -> session_mutex ordering is uniform and never cycles.
    std::shared_mutex session_mutex;
    SnapshotTable snapshots;
    OverlayTable overlays;
    SessionTable sessions;
  };

  static void OnQuery(const std::shared_ptr<State>& state, TransportQuery& query);
  static void OnSample(const std::shared_ptr<State>& state, const TransportSample& sample);
  // Answers a get in the session or snap scope from the matching overlay or
  // snapshot; replies nothing for an unknown sid.
  static void ReplyScopedQuery(const std::shared_ptr<State>& state, std::string_view scope,
                               std::string_view tail, TransportQuery& query);
  // Answers a get on meta/session/<sid> with the session metadata JSON.
  static void ReplyMetaQuery(const std::shared_ptr<State>& state, TransportQuery& query);

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
