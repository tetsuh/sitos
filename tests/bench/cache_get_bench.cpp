// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

class BenchmarkTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view query, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    if (query.find("/session/") != std::string_view::npos) return sitos::Result<void>::Ok();
    for (const auto& reply : replies) sink(reply.first, reply.second, encoding);
    return sitos::Result<void>::Ok();
  }
  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)>) override {
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }
  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

  std::vector<std::pair<std::string, std::vector<std::byte>>> replies;
  sitos::Encoding encoding{std::string(sitos::Encoding::kSitosV1)};
};

struct BenchmarkState {
  std::shared_ptr<BenchmarkTransport> transport;
  sitos::ParamCache cache;
  std::vector<std::string> keys;
};

BenchmarkState& State() {
  static BenchmarkState state = [] {
    auto transport = std::make_shared<BenchmarkTransport>();
    std::vector<std::string> keys;
    keys.reserve(10000);
    transport->replies.reserve(10000);
    for (int index = 0; index < 10000; ++index) {
      auto key = std::string("sitos/snap/s1/key") + std::to_string(index);
      keys.push_back(std::string("key") + std::to_string(index));
      transport->replies.emplace_back(key, sitos::ParamValue(index).Encode());
    }
    auto opened = sitos::ParamCache::Open(transport);
    if (!opened.IsOk()) std::abort();
    auto cache = std::move(opened).Value();
    if (!cache.Attach("s1").IsOk()) std::abort();
    return BenchmarkState{std::move(transport), std::move(cache), std::move(keys)};
  }();
  return state;
}

void BM_ParamCacheGetScalar(benchmark::State& state) {
  auto& fixture = State();
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations() % 10000)];
    auto result = fixture.cache.Get<std::int64_t>(key);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ParamCacheGetScalar)->Arg(10000);

void BM_ParamCacheGetSpan(benchmark::State& state) {
  auto& fixture = State();
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations() % 10000)];
    auto result = fixture.cache.GetSpan<std::byte>(key);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ParamCacheGetSpan)->Arg(10000);

}  // namespace

BENCHMARK_MAIN();
