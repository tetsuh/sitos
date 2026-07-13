// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Session table types used by StorageNode to manage engine-native snapshots
// (snap/<sid>/**), per-session overlays (session/<sid>/**), and session
// metadata (meta/session/<sid>).
// See docs/02_architecture.md §4.1-4.3 and docs/03_wire_protocol.md §7.

#ifndef SITOS_SESSION_HPP
#define SITOS_SESSION_HPP

#include <memory>
#include <string>
#include <unordered_map>

#include "sitos/storage_engine.hpp"

namespace sitos {

/// Metadata recorded for an active session and surfaced as the payload-v1 STR
/// JSON returned for a get on meta/session/<sid> (docs/03 §7.1).
struct SessionMeta {
  /// ISO-8601 UTC timestamp captured at CreateSession, e.g. 2026-07-14T01:23:45Z.
  std::string created_at;
};

/// sid -> engine-native snapshot taken at CreateSession. Reads for
/// snap/<sid>/** resolve against this consistent, immutable view.
using SnapshotTable = std::unordered_map<std::string, std::shared_ptr<const StorageReader>>;

/// sid -> per-session overlay engine. Writes to session/<sid>/** land here and
/// reads for the same scope resolve from it.
using OverlayTable = std::unordered_map<std::string, std::shared_ptr<StorageEngine>>;

/// sid -> session metadata. Membership is the source of truth for whether a
/// session is active.
using SessionTable = std::unordered_map<std::string, SessionMeta>;

}  // namespace sitos

#endif  // SITOS_SESSION_HPP
