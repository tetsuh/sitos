// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageEngine abstraction — the persistence layer that StorageNode reads
// from and writes to.  Engine implementations (InMemory, RocksDB, …)
// inherit from StorageEngine and plug in at construction time.
//
// See docs/02_architecture.md §3.

#ifndef SITOS_STORAGE_ENGINE_HPP
#define SITOS_STORAGE_ENGINE_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace sitos {

/// A view into a value stored in the engine.  The pointed-to bytes are
/// valid only during the sink callback.
using Bytes = std::span<const std::byte>;

/// Callback type for Get and List.  Receives the key and a span over the
/// value bytes.  Return false from the sink to abort iteration early
/// (List will stop and return false).
using EntrySink = std::function<bool(std::string_view key, Bytes value)>;

/// Read-only view of the stored state.  Both the engine itself and
/// lightweight snapshots implement this interface.
class StorageReader {
 public:
  virtual ~StorageReader() = default;

  /// If the key exists, call sink once with the key and its value bytes,
  /// then return true.  If the key does not exist, return false without
  /// calling sink.
  virtual bool Get(std::string_view key, const EntrySink& sink) const = 0;

  /// Enumerate every entry whose key starts with prefix (prefix match on
  /// the key string).  If sink returns false, abort iteration and return
  /// false.  If iteration completes, return true.
  virtual bool List(std::string_view prefix, const EntrySink& sink) const = 0;
};

/// Mutable storage engine with optional snapshot support.
///
/// Thread safety (N07): implementations must guarantee that concurrent
/// calls to Get/List are safe, and that a write (Put/Delete) does not
/// corrupt concurrent reads.
class StorageEngine : public StorageReader {
 public:
  /// Store a value for the given key.  Returns true on success.
  virtual bool Put(std::string_view key, Bytes value) = 0;

  /// Remove the key and its value.  Deleting a non-existent key is a
  /// no-op and should return true.
  virtual bool Delete(std::string_view key) = 0;

  /// Return a consistent read-only view of the state at this point in
  /// time.  The returned snapshot is not affected by subsequent Put or
  /// Delete calls.
  ///
  /// The default implementation copies every entry via List (O(n), N03).
  /// Engines with native snapshot support (e.g. RocksDB) override this
  /// with an O(1) implementation (N02).
  virtual std::shared_ptr<const StorageReader> TakeSnapshot() const;
};

}  // namespace sitos

#endif  // SITOS_STORAGE_ENGINE_HPP
