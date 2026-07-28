// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <atomic>
#include <filesystem>
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
  if (!result.IsOk()) state.SkipWithError("RocksDBEngine::Open failed");
  auto engine = result.IsOk() ? std::move(result).Value() : nullptr;
  if (engine != nullptr) {
    const std::vector<std::byte> value{std::byte{0x01}};
    for (int i = 0; i < state.range(0); ++i) {
      engine->Put("key/" + std::to_string(i), value);
    }
    for (auto _ : state) {
      static_cast<void>(_);
      benchmark::DoNotOptimize(engine->TakeSnapshot());
    }
  }
  engine.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

}  // namespace

BENCHMARK(TakeSnapshot)->Args({1000})->Args({100000});
BENCHMARK_MAIN();
