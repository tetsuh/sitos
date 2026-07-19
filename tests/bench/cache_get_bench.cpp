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
    for (const auto& reply : replies) {
      if (!sink(reply.first, reply.second, encoding)) break;
    }
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
  std::shared_ptr<BenchmarkTransport> scalar_transport;
  std::shared_ptr<BenchmarkTransport> span_transport;
  sitos::ParamCache scalar_cache;
  sitos::ParamCache span_cache;
  std::vector<std::string> scalar_keys;
  std::vector<std::string> span_keys;
  DirectMap scalar_values;
  DirectMap span_values;
};

BenchmarkState& State() {
  static BenchmarkState state = [] {
    constexpr std::size_t kKeyCount = 10000;
    auto scalar_transport = std::make_shared<BenchmarkTransport>();
    auto span_transport = std::make_shared<BenchmarkTransport>();
    std::vector<std::string> scalar_keys;
    std::vector<std::string> span_keys;
    DirectMap scalar_values;
    DirectMap span_values;
    scalar_keys.reserve(kKeyCount);
    span_keys.reserve(kKeyCount);
    scalar_values.reserve(kKeyCount);
    span_values.reserve(kKeyCount);
    scalar_transport->replies.reserve(kKeyCount);
    span_transport->replies.reserve(kKeyCount);

    for (std::size_t index = 0; index < kKeyCount; ++index) {
      const auto suffix = std::to_string(index);
      const auto scalar_key = std::string("key") + suffix;
      const auto span_key = std::string("key") + suffix;
      auto scalar_value = std::make_shared<const sitos::ParamValue>(static_cast<std::int64_t>(index));
      auto bytes = std::vector<std::byte>(16, std::byte{0});
      bytes.front() = static_cast<std::byte>(index & 0xffU);
      auto span_value = std::make_shared<const sitos::ParamValue>(std::move(bytes));
      scalar_keys.push_back(scalar_key);
      span_keys.push_back(span_key);
      scalar_transport->replies.emplace_back("sitos/snap/s1/" + scalar_key,
                                              scalar_value->Encode());
      span_transport->replies.emplace_back("sitos/snap/s1/" + span_key, span_value->Encode());
      scalar_values.emplace(scalar_key, std::move(scalar_value));
      span_values.emplace(span_key, std::move(span_value));
    }

    auto scalar_opened = sitos::ParamCache::Open(scalar_transport);
    auto span_opened = sitos::ParamCache::Open(span_transport);
    if (!scalar_opened.IsOk() || !span_opened.IsOk()) std::abort();
    auto scalar_cache = std::move(scalar_opened).Value();
    auto span_cache = std::move(span_opened).Value();
    if (!scalar_cache.Attach("s1").IsOk() || !span_cache.Attach("s1").IsOk()) std::abort();
    return BenchmarkState{std::move(scalar_transport), std::move(span_transport),
                          std::move(scalar_cache), std::move(span_cache), std::move(scalar_keys),
                          std::move(span_keys), std::move(scalar_values), std::move(span_values)};
  }();
  return state;
}

void BM_ParamCacheGetScalar(benchmark::State& state) {
  auto& fixture = State();
  const auto get_count = fixture.scalar_transport->get_count;
  const auto key_count = static_cast<std::size_t>(state.range(0));
  std::size_t index = 0;
  for (auto _ : state) {
    const auto& key = fixture.scalar_keys[index++ % key_count];
    auto result = fixture.scalar_cache.Get<std::int64_t>(key);
    if (!result.IsOk()) {
      state.SkipWithError("scalar Get failed");
      continue;
    }
    benchmark::DoNotOptimize(result.Value());
  }
  if (fixture.scalar_transport->get_count != get_count) state.SkipWithError("Get used Transport");
}
BENCHMARK(BM_ParamCacheGetScalar)->Arg(10000);

void BM_DirectLookupScalar(benchmark::State& state) {
  auto& fixture = State();
  const auto key_count = static_cast<std::size_t>(state.range(0));
  std::size_t index = 0;
  for (auto _ : state) {
    const auto& key = fixture.scalar_keys[index++ % key_count];
    const auto value = fixture.scalar_values.find(key);
    if (value == fixture.scalar_values.end()) {
      state.SkipWithError("scalar direct lookup failed");
      continue;
    }
    benchmark::DoNotOptimize(value->second->As<std::int64_t>());
  }
}
BENCHMARK(BM_DirectLookupScalar)->Arg(10000);

void BM_ParamCacheGetSpan(benchmark::State& state) {
  auto& fixture = State();
  const auto get_count = fixture.span_transport->get_count;
  const auto key_count = static_cast<std::size_t>(state.range(0));
  std::size_t index = 0;
  for (auto _ : state) {
    const auto& key = fixture.span_keys[index++ % key_count];
    auto result = fixture.span_cache.GetSpan<std::byte>(key);
    if (!result.IsOk()) {
      state.SkipWithError("span Get failed");
      continue;
    }
    const auto span = result.Value().span;
    benchmark::DoNotOptimize(span.data());
    benchmark::DoNotOptimize(span.size());
  }
  if (fixture.span_transport->get_count != get_count) state.SkipWithError("GetSpan used Transport");
}
BENCHMARK(BM_ParamCacheGetSpan)->Arg(10000);

void BM_DirectLookupSpan(benchmark::State& state) {
  auto& fixture = State();
  const auto key_count = static_cast<std::size_t>(state.range(0));
  std::size_t index = 0;
  for (auto _ : state) {
    const auto& key = fixture.span_keys[index++ % key_count];
    const auto value = fixture.span_values.find(key);
    if (value == fixture.span_values.end()) {
      state.SkipWithError("span direct lookup failed");
      continue;
    }
    const auto span = value->second->AsSpan<std::byte>();
    benchmark::DoNotOptimize(span->data());
    benchmark::DoNotOptimize(span->size());
  }
}
BENCHMARK(BM_DirectLookupSpan)->Arg(10000);

}  // namespace

BENCHMARK_MAIN();
