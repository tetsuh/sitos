// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"

#include <mutex>

namespace sitos {

bool InMemoryEngine::Put(std::string_view key, Bytes value) {
  std::unique_lock lock(mutex_);
  data_[std::string(key)] = std::vector<std::byte>(value.begin(), value.end());
  return true;
}

bool InMemoryEngine::Delete(std::string_view key) {
  std::unique_lock lock(mutex_);
  data_.erase(std::string(key));
  return true;
}

bool InMemoryEngine::Get(std::string_view key, const EntrySink& sink) const {
  std::shared_lock lock(mutex_);
  auto it = data_.find(std::string(key));
  if (it == data_.end()) return false;
  sink(it->first, it->second);
  return true;
}

bool InMemoryEngine::List(std::string_view prefix, const EntrySink& sink) const {
  std::shared_lock lock(mutex_);
  for (const auto& [k, v] : data_) {
    if (k.starts_with(prefix) && !sink(k, v)) return false;
  }
  return true;
}

}  // namespace sitos
