// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Legacy Session table aliases retained for source compatibility. StorageNode
// keeps its active ownership and membership in private generation-safe
// SessionRecord instances instead of these aliases.
// See docs/02_architecture.md §4.1-4.3 and docs/03_wire_protocol.md §7.

#ifndef SITOS_SESSION_HPP
#define SITOS_SESSION_HPP

#include <memory>
#include <string>
#include <unordered_map>

#include "sitos/storage_engine.hpp"

namespace sitos {

/// Options selecting Session buffer capabilities.
struct SessionOptions {
  bool durable_buffers = false;
  bool ephemeral_buffers = false;
};

/// Metadata recorded for an active session and surfaced as the payload-v1 STR
/// JSON returned for a get on meta/session/<sid> (docs/03 §7.1).
struct SessionMeta {
  /// ISO-8601 UTC timestamp captured at CreateSession, e.g. 2026-07-14T01:23:45Z.
  std::string created_at;
};

/// Legacy sid -> engine-native snapshot map shape retained for source
/// compatibility; StorageNode does not use this alias as an ownership table.
using SnapshotTable = std::unordered_map<std::string, std::shared_ptr<const StorageReader>>;

/// Legacy sid -> per-session overlay map shape retained for source
/// compatibility; StorageNode does not use this alias as an ownership table.
using OverlayTable = std::unordered_map<std::string, std::shared_ptr<StorageEngine>>;

/// Legacy sid -> session metadata map shape retained for source compatibility;
/// StorageNode does not use this alias as its membership source.
using SessionTable = std::unordered_map<std::string, SessionMeta>;

}  // namespace sitos

#endif  // SITOS_SESSION_HPP
