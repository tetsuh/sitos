// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageEngine contract tests instantiated with a simple mock engine.
// The mock engine stores data in a std::map with a shared_mutex for thread
// safety and uses the default TakeSnapshot() fallback (full copy via List).

#include "storage_engine_contract.hpp"

#include <map>
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
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    sink(it->first, it->second);
    return true;
  }

  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    std::shared_lock lock(mutex_);
    for (const auto& [k, v] : data_) {
      if (k.starts_with(prefix) && !sink(k, v)) return false;
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
