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
  std::string owned_key;
  std::vector<std::byte> owned_value;
  {
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    owned_key = it->first;
    owned_value = it->second;
  }

  sink(owned_key, owned_value);
  return true;
}

bool InMemoryEngine::List(std::string_view prefix, const EntrySink& sink) const {
  std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
  {
    std::shared_lock lock(mutex_);
    for (auto it = data_.lower_bound(prefix);
         it != data_.end() && it->first.starts_with(prefix); ++it) {
      entries.emplace_back(it->first, it->second);
    }
  }

  for (const auto& [key, value] : entries) {
    if (!sink(key, value)) return false;
  }
  return true;
}

}  // namespace sitos
