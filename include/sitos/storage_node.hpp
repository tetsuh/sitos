// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode queryable routing for the base storage scope.

#ifndef SITOS_STORAGE_NODE_HPP
#define SITOS_STORAGE_NODE_HPP

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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
};

/// Serves base-scope Get/List queries through a Transport queryable.
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

  /// Undeclares the queryable. Safe to call repeatedly.
  void Stop() noexcept;

  bool IsStarted() const noexcept { return state_ != nullptr; }

 private:
  struct State {
    State(std::shared_ptr<StorageEngine> storage, std::string key_prefix)
        : engine(std::move(storage)), prefix(std::move(key_prefix)) {}

    std::shared_ptr<StorageEngine> engine;
    std::string prefix;
    std::atomic<bool> active{true};
  };

  static void OnQuery(const std::shared_ptr<State>& state, TransportQuery& query);

  Transport* transport_ = nullptr;
  std::shared_ptr<State> state_;
  Queryable queryable_;
};

}  // namespace sitos

#endif  // SITOS_STORAGE_NODE_HPP
