// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Default TakeSnapshot() implementation — full copy via List (O(n)).

#include "sitos/storage_engine.hpp"

#include <map>
#include <string>
#include <vector>

namespace sitos {
namespace {

/// A lightweight, immutable snapshot that holds a full copy of the state.
class SnapshotCopy : public StorageReader {
 public:
  explicit SnapshotCopy(std::map<std::string, std::vector<std::byte>, std::less<>> data)
      : data_(std::move(data)) {}

  bool Get(std::string_view key, const EntrySink& sink) const override {
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    sink(it->first, it->second);
    return true;
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    for (auto it = data_.lower_bound(prefix);
         it != data_.end() && it->first.starts_with(prefix); ++it) {
      if (!sink(it->first, it->second)) return false;
    }
    return true;
  }

 private:
  std::map<std::string, std::vector<std::byte>, std::less<>> data_;
};

}  // namespace

std::shared_ptr<const StorageReader> StorageEngine::TakeSnapshot() const {
  std::map<std::string, std::vector<std::byte>, std::less<>> data;
  List("", [&data](std::string_view key, Bytes value) {
    data.emplace(std::string(key), std::vector<std::byte>(value.begin(), value.end()));
    return true;
  });
  return std::make_shared<SnapshotCopy>(std::move(data));
}

}  // namespace sitos
