// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode query and subscriber routing for the base storage scope.

#ifndef SITOS_STORAGE_NODE_HPP
#define SITOS_STORAGE_NODE_HPP

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "sitos/logging.hpp"
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
      std::lock_guard<std::mutex> lock(gate_mutex);
      if (!accepting) return std::nullopt;
      ++in_flight;
      return CallbackLease(this);
    }

    void Activate() noexcept {
      std::lock_guard<std::mutex> lock(gate_mutex);
      accepting = true;
    }

    void DeactivateAndWait() noexcept {
      std::unique_lock<std::mutex> lock(gate_mutex);
      accepting = false;
      gate_cv.wait(lock, [this] { return in_flight == 0; });
    }

    void Leave() noexcept {
      std::lock_guard<std::mutex> lock(gate_mutex);
      if (in_flight == 0) return;
      --in_flight;
      if (in_flight == 0) gate_cv.notify_all();
    }

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::size_t in_flight = 0;
    bool accepting = false;
  };

  static void OnQuery(const std::shared_ptr<State>& state, TransportQuery& query);
  static void OnSample(const std::shared_ptr<State>& state, const TransportSample& sample);

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
