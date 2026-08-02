// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "transport/declaration_handle_test_access.hpp"

namespace {

class LateJoinTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions) override {
    sitos::TransportSample sample{std::string(key), payload, std::move(encoding), std::nullopt,
                                  sitos::TransportSample::Kind::Put};
    for (const auto& subscriber : subscribers_) subscriber(sample);
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    if (query_failure) {
      return sitos::Result<void>::Err(std::make_error_code(std::errc::io_error));
    }
    auto query = sitos::TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          if (sink(key, payload, std::move(encoding))) return sitos::Result<void>::Ok();
          return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
        });
    query.keyexpr = std::string(keyexpr);
    if (queryable_) queryable_(query);
    if (block_get) {
      std::unique_lock lock(get_mutex);
      get_entered = true;
      get_cv.notify_all();
      get_cv.wait(lock, [&] { return !block_get; });
    }
    return sitos::Result<void>::Ok();
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    subscribers_.push_back(std::move(callback));
    return sitos::Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)> callback) override {
    queryable_ = std::move(callback);
    return sitos::Result<sitos::Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

 private:
  std::vector<std::function<void(const sitos::TransportSample&)>> subscribers_;
  std::function<void(sitos::TransportQuery&)> queryable_;

 public:
  bool query_failure = false;
  bool block_get = false;
  bool get_entered = false;
  std::mutex get_mutex;
  std::condition_variable get_cv;

  void WaitForGet() {
    std::unique_lock lock(get_mutex);
    get_cv.wait(lock, [&] { return get_entered; });
  }
  void ReleaseGet() {
    {
      std::scoped_lock lock(get_mutex);
      block_get = false;
    }
    get_cv.notify_all();
  }
};

class LateJoinCollector {
 public:
  LateJoinCollector(LateJoinTransport& transport, std::string selector,
                    std::function<void(std::string)> observer)
      : transport_(transport), selector_(std::move(selector)), observer_(std::move(observer)) {}

  bool Join() {
    auto subscription_result = transport_.DeclareSubscriber(
        selector_, [this](const sitos::TransportSample& sample) {
          if (transitioned_) {
            observer_(sample.key);
          } else {
            buffered_.push_back(sample.key);
          }
        });
    if (!subscription_result.IsOk()) return false;
    subscription_ = std::move(subscription_result).Value();

    std::vector<std::string> materialized;
    const auto result = transport_.Get(
        selector_, [&](std::string_view key, std::span<const std::byte>, sitos::Encoding) {
          materialized.emplace_back(key);
          return true;
        },
        std::chrono::seconds(1));
    if (!result.IsOk()) return false;
    for (const auto& key : materialized) observer_(key);
    transitioned_ = true;
    for (const auto& key : buffered_) observer_(key);
    buffered_.clear();
    return true;
  }

 private:
  LateJoinTransport& transport_;
  std::string selector_;
  std::function<void(std::string)> observer_;
  std::optional<sitos::Subscription> subscription_;
  std::vector<std::string> buffered_;
  bool transitioned_ = false;
};

struct LateJoinFixture {
  LateJoinTransport transport;
  std::shared_ptr<sitos::InMemoryEngine> engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node{transport};

  void Start() {
    ASSERT_TRUE(node.Start(
        engine, {.prefix = "sitos",
                 .durable_buffer_engine_factory = [](std::string_view) {
                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                       std::make_unique<sitos::InMemoryEngine>());
                 }}));
    ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  }
};

TEST(BufferLateJoinTest, OrdersMaterializedBufferedAndLiveSamples) {
  LateJoinFixture fixture;
  fixture.Start();
  const std::vector<std::byte> first{std::byte{1}};
  const std::vector<std::byte> second{std::byte{2}};
  ASSERT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/first", first,
                                    {"zenoh/bytes"}, {})
                  .IsOk());
  ASSERT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/second", second,
                                    {"zenoh/bytes"}, {})
                  .IsOk());

  std::vector<std::string> observed;
  LateJoinCollector collector(fixture.transport, "sitos/buffers/session/durable/**",
                               [&](std::string key) { observed.push_back(std::move(key)); });
  fixture.transport.block_get = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForGet();
  const std::vector<std::byte> buffered{std::byte{3}};
  ASSERT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/buffered", buffered,
                                    {"zenoh/bytes"}, {})
                  .IsOk());
  fixture.transport.ReleaseGet();
  join.join();
  ASSERT_TRUE(joined.has_value());
  ASSERT_TRUE(*joined);
  const std::vector<std::byte> live{std::byte{4}};
  ASSERT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/live", live,
                                    {"zenoh/bytes"}, {})
                  .IsOk());

  ASSERT_EQ(observed.size(), 4u);
  EXPECT_EQ(observed[0], "sitos/buffers/session/durable/first");
  EXPECT_EQ(observed[1], "sitos/buffers/session/durable/second");
  EXPECT_EQ(observed[2], "sitos/buffers/session/durable/buffered");
  EXPECT_EQ(observed[3], "sitos/buffers/session/durable/live");
}

TEST(BufferLateJoinTest, DurableLateJoinDoesNotLoseDistinctKeys) {
  LateJoinFixture fixture;
  fixture.Start();
  for (const auto key : {"a", "b", "c"}) {
    const std::vector<std::byte> value{std::byte{1}};
    ASSERT_TRUE(fixture.transport.Put(
                         "sitos/buffers/session/durable/" + std::string(key), value,
                         {"zenoh/bytes"}, {})
                    .IsOk());
  }

  std::vector<std::string> observed;
  LateJoinCollector collector(fixture.transport, "sitos/buffers/session/durable/**",
                               [&](std::string key) { observed.push_back(std::move(key)); });
  ASSERT_TRUE(collector.Join());
  ASSERT_EQ(observed.size(), 3u);
  EXPECT_EQ(std::set<std::string>(observed.begin(), observed.end()),
            (std::set<std::string>{"sitos/buffers/session/durable/a",
                                   "sitos/buffers/session/durable/b",
                                   "sitos/buffers/session/durable/c"}));
}

TEST(BufferLateJoinTest, FailureInvokesNoObserverAndCleansUp) {
  LateJoinFixture fixture;
  fixture.Start();
  const std::vector<std::byte> failing{std::byte{9}};
  ASSERT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/failing", failing,
                                    {"zenoh/bytes"}, {})
                  .IsOk());
  std::vector<std::string> observed;
  fixture.transport.query_failure = true;
  LateJoinCollector collector(fixture.transport, "sitos/buffers/session/durable/**",
                               [&](std::string key) { observed.push_back(std::move(key)); });
  EXPECT_FALSE(collector.Join());
  EXPECT_TRUE(observed.empty());
  EXPECT_TRUE(fixture.node.CloseSession("session").IsOk());
  const std::vector<std::byte> after_close{std::byte{1}};
  EXPECT_TRUE(fixture.transport.Put("sitos/buffers/session/durable/after-close", after_close,
                                     {"zenoh/bytes"}, {})
                  .IsOk());
  EXPECT_TRUE(observed.empty());
}

}  // namespace
