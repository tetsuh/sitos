// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_IN_MEMORY_ENGINE_HPP
#define SITOS_IN_MEMORY_ENGINE_HPP

#include "sitos/storage_engine.hpp"

#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

namespace sitos {

/// A StorageEngine backed by a std::map with std::shared_mutex for thread
/// safety.  Snapshots are full copies (O(n)), produced by the default
/// TakeSnapshot() fallback.
class InMemoryEngine : public StorageEngine {
 public:
  InMemoryEngine() = default;

  bool Put(std::string_view key, Bytes value) override;
  bool Delete(std::string_view key) override;
  bool Get(std::string_view key, const EntrySink& sink) const override;
  bool List(std::string_view prefix, const EntrySink& sink) const override;

 private:
  mutable std::shared_mutex mutex_;
  std::map<std::string, std::vector<std::byte>, std::less<>> data_;
};

}  // namespace sitos

#endif  // SITOS_IN_MEMORY_ENGINE_HPP
