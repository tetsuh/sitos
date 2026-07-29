// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_ROCKSDB_ENGINE_HPP
#define SITOS_ROCKSDB_ENGINE_HPP

#include <memory>
#include <string>

#include "sitos/result.hpp"
#include "sitos/storage_engine.hpp"

namespace sitos {

/// Persistent StorageEngine backed by RocksDB when SITOS_WITH_ROCKSDB is enabled.
/// The public header intentionally contains no RocksDB types.
class RocksDBEngine : public StorageEngine {
 public:
  /// Opens or creates a database at path.
  static Result<std::unique_ptr<RocksDBEngine>> Open(const std::string& path);

  ~RocksDBEngine() override;

  RocksDBEngine(const RocksDBEngine&) = delete;
  RocksDBEngine& operator=(const RocksDBEngine&) = delete;
  RocksDBEngine(RocksDBEngine&&) = delete;
  RocksDBEngine& operator=(RocksDBEngine&&) = delete;

  bool Put(std::string_view key, Bytes value) override;
  bool Delete(std::string_view key) override;
  bool Get(std::string_view key, const EntrySink& sink) const override;
  bool List(std::string_view prefix, const EntrySink& sink) const override;
  std::shared_ptr<const StorageReader> TakeSnapshot() const override;

 private:
  struct Impl;
  explicit RocksDBEngine(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_ROCKSDB_ENGINE_HPP
