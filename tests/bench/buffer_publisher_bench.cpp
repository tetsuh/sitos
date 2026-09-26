// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sitos/buffer_publisher.hpp"
#include "sitos/param_value.hpp"

namespace {

class BenchmarkTransport final : public sitos::Transport {
 public:
  bool SupportsFenceProfile() const noexcept override { return true; }
  std::uint64_t FenceGeneration() const noexcept override { return 1; }

  sitos::Result<void> Put(std::string_view, std::span<const std::byte> payload, sitos::Encoding,
                          sitos::PutOptions) override {
    retained_.assign(payload.begin(), payload.end());
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view key, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    if (key.find("/meta/ack/") != std::string_view::npos) return sitos::Result<void>::Ok();
    const auto value =
        sitos::ParamValue(
            R"({"state":"active","created_at":"benchmark","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})")
            .Encode();
    sink(key, value, sitos::Encoding{std::string(sitos::Encoding::kSitosV1)});
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

 private:
  std::vector<std::byte> retained_;
};

void BufferPublisherPush(benchmark::State& state) {
  const auto pushes_per_second = static_cast<std::size_t>(state.range(0));
  const auto value_bytes = static_cast<std::size_t>(state.range(1));
  state.SetLabel("paced_batch=one_second_target_rate");
  auto transport = std::make_shared<BenchmarkTransport>();
  auto opened = sitos::BufferPublisher::Open(transport, sitos::ClientConfig{}, "bench",
                                             sitos::BufferClass::Ephemeral);
  if (!opened.IsOk()) state.SkipWithError("BufferPublisher open failed");
  auto publisher = std::move(opened).Value();
  std::vector<std::byte> value(value_bytes);
  std::size_t sequence = 0;
  for (auto _ : state) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < pushes_per_second; ++index) {
      const auto result = publisher.Push("value-" + std::to_string(sequence++), value);
      if (!result.IsOk()) state.SkipWithError("BufferPublisher push failed");
      const auto target = start + std::chrono::nanoseconds{static_cast<std::int64_t>(
                                      (index + 1) * 1000000000ULL / pushes_per_second)};
      std::this_thread::sleep_until(target);
    }
    state.SetIterationTime(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * pushes_per_second);
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * pushes_per_second *
                          value_bytes);
}

BENCHMARK(BufferPublisherPush)
    ->Args({3, 256 * 1024})
    ->Args({3, 1024 * 1024})
    ->Args({300, 256 * 1024})
    ->Args({300, 1024 * 1024})
    ->UseManualTime();

}  // namespace

BENCHMARK_MAIN();
