// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

using namespace std::chrono_literals;

class CountingTransport final : public sitos::Transport {
 public:
  explicit CountingTransport(std::shared_ptr<sitos::Transport> inner) : inner_(std::move(inner)) {}

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    ++put_count;
    return inner_->Put(key, payload, std::move(encoding), std::move(options));
  }

  sitos::Result<void> Delete(std::string_view key, sitos::PutOptions options) override {
    return inner_->Delete(key, std::move(options));
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    return inner_->Get(keyexpr, sink, timeout);
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    return inner_->DeclareSubscriber(keyexpr, std::move(callback));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view keyexpr, std::function<void(sitos::TransportQuery&)> callback) override {
    return inner_->DeclareQueryable(keyexpr, std::move(callback));
  }

  std::atomic<int> put_count = 0;

 private:
  std::shared_ptr<sitos::Transport> inner_;
};

TEST(ParamStoreAckIntegrationTest, QualifiesApplicationTimeoutAndReuse) {
  constexpr std::string_view prefix = "sitos/param_store_ack_integration";
  auto transport = std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
  ASSERT_TRUE(transport);
  auto counting = std::make_shared<CountingTransport>(transport);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node(*transport);
  ASSERT_TRUE(node.Start(engine, *transport, {.prefix = std::string(prefix)}).IsOk());

  sitos::ClientConfig config;
  config.prefix = std::string(prefix);
  config.query_timeout = 500ms;
  auto store_result = sitos::ParamStore::Open(counting, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  const auto first_puts = counting->put_count.load();
  ASSERT_TRUE(store.Put("base", "value", std::int64_t{7}).IsOk());
  EXPECT_EQ(counting->put_count.load(), first_puts + 1);
  std::int64_t observed_value = 0;
  ASSERT_TRUE(engine->Get("value", [&](std::string_view, sitos::Bytes bytes) {
    auto decoded = sitos::ParamValue::Decode(bytes);
    if (decoded.has_value()) observed_value = decoded->As<std::int64_t>().value_or(-1);
    return true;
  }));
  EXPECT_EQ(observed_value, 7);
  const std::vector<sitos::BatchEntry> entries = {{"first", sitos::ParamValue(std::int64_t{1})},
                                                  {"second", sitos::ParamValue(std::int64_t{2})}};
  const auto batch_puts = counting->put_count.load();
  ASSERT_TRUE(store.PutBatch("base", entries).IsOk());
  EXPECT_EQ(counting->put_count.load(), batch_puts + 1);
  std::int64_t observed_first = 0;
  std::int64_t observed_second = 0;
  ASSERT_TRUE(engine->Get("first", [&](std::string_view, sitos::Bytes bytes) {
    auto decoded = sitos::ParamValue::Decode(bytes);
    if (decoded.has_value()) observed_first = decoded->As<std::int64_t>().value_or(-1);
    return true;
  }));
  ASSERT_TRUE(engine->Get("second", [&](std::string_view, sitos::Bytes bytes) {
    auto decoded = sitos::ParamValue::Decode(bytes);
    if (decoded.has_value()) observed_second = decoded->As<std::int64_t>().value_or(-1);
    return true;
  }));
  EXPECT_EQ(observed_first, 1);
  EXPECT_EQ(observed_second, 2);

  node.Stop();
  const auto timeout_puts = counting->put_count.load();
  const auto started = std::chrono::steady_clock::now();
  auto timeout = store.Put("base", "offline", std::int64_t{8},
                           sitos::ParamStore::WriteOptions{.ack = true, .ack_timeout = 150ms});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT_FALSE(timeout.IsOk());
  EXPECT_EQ(timeout.StatusCode(), sitos::Status::Timeout);
  EXPECT_GE(elapsed, 150ms);
  EXPECT_EQ(counting->put_count.load(), timeout_puts + 1);

  const auto submission_puts = counting->put_count.load();
  auto submission_only =
      store.Put("base", "submission-only", std::int64_t{10},
                sitos::ParamStore::WriteOptions{.ack = false, .ack_timeout = 0ms});
  ASSERT_TRUE(submission_only.IsOk()) << submission_only.Message();
  EXPECT_EQ(counting->put_count.load(), submission_puts + 1);

  auto replacement_engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode replacement(*transport);
  ASSERT_TRUE(
      replacement.Start(replacement_engine, *transport, {.prefix = std::string(prefix)}).IsOk());
  const auto reuse_puts = counting->put_count.load();
  auto reused = store.Put("base", "reused", std::int64_t{9});
  EXPECT_TRUE(reused.IsOk()) << reused.Message();
  EXPECT_EQ(counting->put_count.load(), reuse_puts + 1);
  replacement.Stop();
}

}  // namespace
