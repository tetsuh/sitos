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
#include <unordered_map>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

struct TransparentStringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
  std::size_t operator()(const std::string& value) const noexcept {
    return operator()(std::string_view(value));
  }
};

struct TransparentStringEqual {
  using is_transparent = void;
  bool operator()(std::string_view left, std::string_view right) const noexcept {
    return left == right;
  }
  bool operator()(const std::string& left, std::string_view right) const noexcept {
    return left == right;
  }
  bool operator()(std::string_view left, const std::string& right) const noexcept {
    return left == right;
  }
  bool operator()(const std::string& left, const std::string& right) const noexcept {
    return left == right;
  }
};

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
    ++get_count;
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

  std::size_t get_count = 0;
  std::vector<std::pair<std::string, std::vector<std::byte>>> replies;
  sitos::Encoding encoding{std::string(sitos::Encoding::kSitosV1)};
};

using DirectMap = std::unordered_map<std::string, std::shared_ptr<const sitos::ParamValue>,
                                     TransparentStringHash, TransparentStringEqual>;

struct BenchmarkState {
  std::shared_ptr<BenchmarkTransport> transport;
  sitos::ParamCache cache;
  std::vector<std::string> keys;
  DirectMap direct_values;
};

BenchmarkState& State() {
  static BenchmarkState state = [] {
    auto transport = std::make_shared<BenchmarkTransport>();
    std::vector<std::string> keys;
    keys.reserve(10000);
    transport->replies.reserve(10000);
    DirectMap direct_values;
    direct_values.reserve(10000);
    for (int index = 0; index < 10000; ++index) {
      auto key = std::string("sitos/snap/s1/key") + std::to_string(index);
      auto relative_key = std::string("key") + std::to_string(index);
      auto value = std::make_shared<const sitos::ParamValue>(index);
      keys.push_back(std::move(relative_key));
      transport->replies.emplace_back(key, value->Encode());
      direct_values.emplace(keys.back(), std::move(value));
    }
    auto opened = sitos::ParamCache::Open(transport);
    if (!opened.IsOk()) std::abort();
    auto cache = std::move(opened).Value();
    if (!cache.Attach("s1").IsOk()) std::abort();
    return BenchmarkState{std::move(transport), std::move(cache), std::move(keys),
                          std::move(direct_values)};
  }();
  return state;
}

void BM_ParamCacheGetScalar(benchmark::State& state) {
  auto& fixture = State();
  const auto get_count = fixture.transport->get_count;
  const auto key_count = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations()) % key_count];
    auto result = fixture.cache.Get<std::int64_t>(key);
    benchmark::DoNotOptimize(result);
  }
  if (fixture.transport->get_count != get_count) state.SkipWithError("Get used Transport");
}
BENCHMARK(BM_ParamCacheGetScalar)->Arg(10000);

void BM_DirectLookupScalar(benchmark::State& state) {
  auto& fixture = State();
  const auto key_count = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations()) % key_count];
    const auto value = fixture.direct_values.find(key);
    benchmark::DoNotOptimize(value->second->As<std::int64_t>());
  }
}
BENCHMARK(BM_DirectLookupScalar)->Arg(10000);

void BM_ParamCacheGetSpan(benchmark::State& state) {
  auto& fixture = State();
  const auto get_count = fixture.transport->get_count;
  const auto key_count = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations()) % key_count];
    auto result = fixture.cache.GetSpan<std::byte>(key);
    benchmark::DoNotOptimize(result);
  }
  if (fixture.transport->get_count != get_count) state.SkipWithError("GetSpan used Transport");
}
BENCHMARK(BM_ParamCacheGetSpan)->Arg(10000);

void BM_DirectLookupSpan(benchmark::State& state) {
  auto& fixture = State();
  const auto key_count = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    const auto& key = fixture.keys[static_cast<std::size_t>(state.iterations()) % key_count];
    const auto value = fixture.direct_values.find(key);
    benchmark::DoNotOptimize(value->second->AsSpan<std::byte>());
  }
}
BENCHMARK(BM_DirectLookupSpan)->Arg(10000);

}  // namespace

BENCHMARK_MAIN();
