// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ADR-0038 and ADR-0039: a Zenoh session can open before the StorageNode is connected, and a
// sample or query sent then reaches no StorageNode. Clients wait, within the operation's own
// deadline, until a StorageNode queryable for the prefix is reachable. Internal to sitos; not an
// installed header.

#ifndef SITOS_STORAGE_NODE_REACHABILITY_HPP
#define SITOS_STORAGE_NODE_REACHABILITY_HPP

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

#include "sitos/transport.hpp"

namespace sitos::storage_node_reachability {

using Clock = std::chrono::steady_clock;

/// Waits until a StorageNode queryable for `prefix` is reachable or `deadline` passes. Only
/// StorageNode declares queryables, so every caller uses the same key expression for a prefix and
/// the Zenoh adapter keeps one querier for it. The result is ignored: an unmatched or failed wait
/// still lets the caller submit or query, which reports its own outcome.
inline void WaitForStorageNode(Transport& transport, std::string_view prefix,
                               Clock::time_point deadline) {
  static_cast<void>(
      transport.WaitForMatchingQueryable(std::string(prefix) + "/meta/ack/*", deadline));
}

/// Waits within `timeout` and returns the time left for the query that follows. The result is at
/// least 1 ms, because Transport::Get rejects a non-positive timeout, so the wait and the query
/// together take about `timeout`.
inline std::chrono::milliseconds WaitForStorageNodeWithin(Transport& transport,
                                                          std::string_view prefix,
                                                          std::chrono::milliseconds timeout) {
  const auto now = Clock::now();
  const auto room =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
  const auto deadline =
      timeout >= room ? Clock::time_point::max()
                      : now + std::chrono::duration_cast<Clock::duration>(timeout);
  WaitForStorageNode(transport, prefix, deadline);
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
  return std::clamp(left, std::chrono::milliseconds{1}, timeout);
}

}  // namespace sitos::storage_node_reachability

#endif  // SITOS_STORAGE_NODE_REACHABILITY_HPP
