// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageEngine contract tests instantiated with a simple mock engine.
// The mock engine stores data in a std::map with a shared_mutex for thread
// safety and uses the default TakeSnapshot() fallback (full copy via List).

#include "storage_engine_contract.hpp"

#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace {

/// A minimal engine that stores key-value pairs in a std::map.
/// Uses std::shared_mutex to satisfy the N07 concurrency requirement.
class MockEngine : public sitos::StorageEngine {
 public:
  bool Put(std::string_view key, sitos::Bytes value) override {
    std::unique_lock lock(mutex_);
    data_[std::string(key)] = std::vector<std::byte>(value.begin(), value.end());
    return true;
  }

  bool Delete(std::string_view key) override {
    std::unique_lock lock(mutex_);
    data_.erase(std::string(key));
    return true;
  }

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
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

  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    {
      std::shared_lock lock(mutex_);
      for (const auto& [key, value] : data_) {
        if (key.starts_with(prefix)) entries.emplace_back(key, value);
      }
    }

    for (const auto& [key, value] : entries) {
      if (!sink(key, value)) return false;
    }
    return true;
  }

 private:
  mutable std::shared_mutex mutex_;
  std::map<std::string, std::vector<std::byte>, std::less<>> data_;
};

}  // namespace

INSTANTIATE_STORAGE_ENGINE_CONTRACT_SUITE(MockEngineContractTest, [] {
  return std::make_unique<MockEngine>();
});
