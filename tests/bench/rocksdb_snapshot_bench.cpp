// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sitos/rocksdb_engine.hpp"

namespace {

std::filesystem::path MakePath() {
  static std::atomic<unsigned int> next_id{0};
  return std::filesystem::temp_directory_path() /
         ("sitos-rocksdb-bench-" + std::to_string(next_id.fetch_add(1)));
}

void TakeSnapshot(benchmark::State& state) {
  const auto path = MakePath();
  auto result = sitos::RocksDBEngine::Open(path.string());
  if (!result.IsOk()) {
    state.SkipWithError("RocksDBEngine::Open failed");
    return;
  }
  auto engine = std::move(result).Value();
  const std::vector<std::byte> value{std::byte{0x01}};
  for (int i = 0; i < state.range(0); ++i) {
    if (!engine->Put("key/" + std::to_string(i), value)) {
      state.SkipWithError("RocksDBEngine::Put failed");
      engine.reset();
      std::error_code error;
      std::filesystem::remove_all(path, error);
      return;
    }
  }
  for (auto _ : state) {
    static_cast<void>(_);
    auto snapshot = engine->TakeSnapshot();
    if (!snapshot) {
      state.SkipWithError("RocksDBEngine::TakeSnapshot returned null");
      break;
    }
    benchmark::DoNotOptimize(snapshot.get());
  }
  engine.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) state.SkipWithError("RocksDB cleanup failed");
}

}  // namespace

BENCHMARK(TakeSnapshot)->Name("TakeSnapshot")->Args({1000})->Args({100000});
BENCHMARK_MAIN();
