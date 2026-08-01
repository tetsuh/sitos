// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_STORAGE_NODE_TEST_ACCESS_HPP
#define SITOS_STORAGE_NODE_TEST_ACCESS_HPP

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "sitos/storage_engine.hpp"
#include "sitos/storage_node.hpp"

namespace sitos::storage_node_test_access {

/// Source-private access used only by StorageNode lifecycle tests.
struct SessionResourceObservation {
  std::weak_ptr<void> record;
  std::weak_ptr<const void> snapshot;
  std::weak_ptr<void> overlay;
};

class StorageNodeTestAccess {
 public:
  // Non-owning: the test must capture this while node.state_ is published and
  // keep an admitted operation alive until WaitForClosed() completes.
  struct GateObserver {
    StorageNode::State* state = nullptr;

    bool WaitForClosed() const {
      if (state == nullptr) return false;
      std::unique_lock lock(state->gate_mutex);
      state->gate_cv.wait(lock, [this] { return !state->accepting; });
      return state->in_flight > 0;
    }
  };

  static std::optional<GateObserver> CaptureGateObserver(StorageNode& node) {
    std::scoped_lock lock(node.lifecycle_mutex_);
    if (node.state_ == nullptr) return std::nullopt;
    return GateObserver{node.state_.get()};
  }

  static std::optional<SessionResourceObservation> ObserveSession(StorageNode& node,
                                                                    std::string_view sid) {
    std::shared_ptr<StorageNode::State> state;
    {
      std::scoped_lock lock(node.lifecycle_mutex_);
      state = node.state_;
    }
    if (state == nullptr) return std::nullopt;
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(sid);
    if (it == state->sessions.end()) return std::nullopt;
    SessionResourceObservation observation;
    observation.record = it->second;
    observation.snapshot = it->second->snapshot;
    observation.overlay = it->second->overlay;
    return observation;
  }

  static bool WaitForClosing(StorageNode& node, std::string_view sid) {
    std::shared_ptr<StorageNode::State> state;
    {
      std::scoped_lock lock(node.lifecycle_mutex_);
      state = node.state_;
    }
    if (state == nullptr) return false;
    std::shared_ptr<StorageNode::SessionRecord> record;
    {
      std::shared_lock state_lock(state->session_mutex);
      auto it = state->sessions.find(sid);
      if (it == state->sessions.end()) return false;
      record = it->second;
    }
    std::unique_lock lock(record->admission_mutex);
    record->admission_cv.wait(lock, [&] {
      return record->phase == StorageNode::SessionRecord::Phase::Closing;
    });
    return true;
  }

  static bool ReplaceSessionOverlay(StorageNode& node, std::string_view sid,
                                    std::shared_ptr<StorageEngine> overlay) {
    std::shared_ptr<StorageNode::State> state;
    {
      std::scoped_lock lock(node.lifecycle_mutex_);
      state = node.state_;
    }
    if (state == nullptr) return false;
    std::unique_lock lock(state->session_mutex);
    auto it = state->sessions.find(sid);
    if (it == state->sessions.end() ||
        it->second->phase != StorageNode::SessionRecord::Phase::Active) {
      return false;
    }
    it->second->overlay = std::move(overlay);
    return true;
  }
};

}  // namespace sitos::storage_node_test_access

#endif  // SITOS_STORAGE_NODE_TEST_ACCESS_HPP
