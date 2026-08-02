// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace {

struct Observation {
  std::string key;
  std::vector<std::byte> payload;
  std::string encoding;
};

class LateJoinTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions) override {
    sitos::TransportSample sample{std::string(key), payload, std::move(encoding), std::nullopt,
                                  sitos::TransportSample::Kind::Put};
    std::vector<std::function<void(const sitos::TransportSample&)>> callbacks;
    {
      std::scoped_lock lock(subscriber_mutex_);
      for (const auto& [id, callback] : subscribers_) {
        static_cast<void>(id);
        callbacks.push_back(callback);
      }
    }
    for (const auto& callback : callbacks) callback(sample);
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    if (query_failure) return sitos::Result<void>::Err(std::make_error_code(std::errc::io_error));
    int reply_count = 0;
    auto query = sitos::TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          ++reply_count;
          if (fail_after_replies >= 0 && reply_count > fail_after_replies) {
            return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
          }
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
    if (fail_after_replies >= 0 && reply_count > fail_after_replies) {
      return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
    }
    return sitos::Result<void>::Ok();
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    if (declaration_failure) {
      return sitos::Result<sitos::Subscription>::Err(
          std::make_error_code(std::errc::permission_denied));
    }
    const int id = next_subscriber_id++;
    {
      std::scoped_lock lock(subscriber_mutex_);
      subscribers_.emplace(id, std::move(callback));
    }
    return sitos::Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([this, id] {
          std::scoped_lock lock(subscriber_mutex_);
          subscribers_.erase(id);
        }));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)> callback) override {
    queryable_ = std::move(callback);
    return sitos::Result<sitos::Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

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

  bool query_failure = false;
  bool declaration_failure = false;
  int fail_after_replies = -1;
  bool block_get = false;
  bool get_entered = false;
  std::mutex get_mutex;
  std::condition_variable get_cv;

 private:
  int next_subscriber_id = 1;
  std::mutex subscriber_mutex_;
  std::map<int, std::function<void(const sitos::TransportSample&)>> subscribers_;
  std::function<void(sitos::TransportQuery&)> queryable_;
};

class LateJoinCollector final {
 public:
  LateJoinCollector(LateJoinTransport& transport, std::string selector,
                    std::function<void(const Observation&)> observer)
      : transport_(transport), selector_(std::move(selector)), observer_(std::move(observer)) {}

  bool Join() {
    auto declaration = transport_.DeclareSubscriber(
        selector_, [this](const sitos::TransportSample& sample) { OnSample(sample); });
    if (!declaration.IsOk()) {
      std::scoped_lock lock(collector_mutex_);
      phase_ = Phase::Failed;
      return false;
    }
    {
      std::scoped_lock lock(collector_mutex_);
      subscription_ = std::move(declaration).Value();
    }

    const auto result = transport_.Get(
        selector_,
        [this](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          Observation observation{
              std::string(key), {payload.begin(), payload.end()}, std::move(encoding.id)};
          std::scoped_lock lock(collector_mutex_);
          return AddObservation(observation);
        },
        std::chrono::seconds(1));
    std::vector<Observation> batch;
    {
      std::unique_lock lock(collector_mutex_);
      if (!result.IsOk() || phase_ == Phase::Failed) {
        phase_ = Phase::Failed;
        subscription_.reset();
        pending_.clear();
        return false;
      }
      observation_mutex_.lock();
      phase_ = Phase::Live;
      batch = std::move(pending_);
      pending_.clear();
    }
    DispatchAndUnlock(std::move(batch));
    return true;
  }

  bool Failed() const {
    std::scoped_lock lock(collector_mutex_);
    return phase_ == Phase::Failed;
  }

  ~LateJoinCollector() {
    std::scoped_lock lock(collector_mutex_);
    phase_ = Phase::Failed;
    subscription_.reset();
    pending_.clear();
  }

 private:
  enum class Phase { Collecting, Live, Failed };

  static bool Same(const Observation& lhs, const Observation& rhs) {
    return lhs.key == rhs.key && lhs.payload == rhs.payload && lhs.encoding == rhs.encoding;
  }

  bool AddObservation(const Observation& observation) {
    auto it = seen_.find(observation.key);
    if (it != seen_.end()) return Same(it->second, observation);
    seen_.emplace(observation.key, observation);
    pending_.push_back(observation);
    return true;
  }

  void OnSample(const sitos::TransportSample& sample) {
    Observation observation{
        sample.key, {sample.payload.begin(), sample.payload.end()}, sample.encoding.id};
    std::vector<Observation> dispatch;
    {
      std::unique_lock lock(collector_mutex_);
      if (phase_ == Phase::Failed) return;
      if (phase_ == Phase::Collecting) {
        if (!AddObservation(observation)) phase_ = Phase::Failed;
        return;
      }
      if (const auto it = seen_.find(observation.key); it != seen_.end()) {
        if (!Same(it->second, observation)) phase_ = Phase::Failed;
        return;
      }
      if (!AddObservation(observation)) {
        phase_ = Phase::Failed;
        return;
      }
      dispatch.push_back(observation);
      observation_mutex_.lock();
    }
    for (const auto& item : dispatch) observer_(item);
    observation_mutex_.unlock();
  }

  void DispatchAndUnlock(std::vector<Observation> batch) {
    for (const auto& observation : batch) observer_(observation);
    observation_mutex_.unlock();
  }

  LateJoinTransport& transport_;
  std::string selector_;
  std::function<void(const Observation&)> observer_;
  mutable std::mutex collector_mutex_;
  std::mutex observation_mutex_;
  std::optional<sitos::Subscription> subscription_;
  std::map<std::string, Observation> seen_;
  std::vector<Observation> pending_;
  Phase phase_ = Phase::Collecting;
};

struct LateJoinFixture {
  LateJoinTransport transport;
  std::shared_ptr<sitos::InMemoryEngine> engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node{transport};

  void Start() {
    ASSERT_TRUE(node.Start(
        engine, {.prefix = "sitos", .durable_buffer_engine_factory = [](std::string_view) {
                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                       std::make_unique<sitos::InMemoryEngine>());
                 }}));
    ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  }
};

std::vector<std::byte> Bytes(std::initializer_list<unsigned char> values) {
  std::vector<std::byte> result;
  for (const auto value : values) result.push_back(std::byte{value});
  return result;
}

TEST(BufferLateJoinTest, OrdersMaterializedBufferedAndLiveSamples) {
  LateJoinFixture fixture;
  fixture.Start();
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/first", Bytes({1}), {"zenoh/bytes"}, {})
          .IsOk());
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/second", Bytes({2}), {"zenoh/bytes"}, {})
          .IsOk());

  std::vector<Observation> observed;
  LateJoinCollector collector(
      fixture.transport, "sitos/buffers/session/durable/**",
      [&](const Observation& observation) { observed.push_back(observation); });
  fixture.transport.block_get = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForGet();
  ASSERT_TRUE(fixture.transport
                  .Put("sitos/buffers/session/durable/buffered", Bytes({3}), {"zenoh/bytes"}, {})
                  .IsOk());
  fixture.transport.ReleaseGet();
  join.join();
  ASSERT_TRUE(joined.has_value());
  ASSERT_TRUE(*joined);
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/live", Bytes({4}), {"zenoh/bytes"}, {})
          .IsOk());

  ASSERT_EQ(observed.size(), 4u);
  EXPECT_EQ(observed[0].key, "sitos/buffers/session/durable/first");
  EXPECT_EQ(observed[1].key, "sitos/buffers/session/durable/second");
  EXPECT_EQ(observed[2].key, "sitos/buffers/session/durable/buffered");
  EXPECT_EQ(observed[3].key, "sitos/buffers/session/durable/live");
  EXPECT_EQ(observed[2].payload, Bytes({3}));
  EXPECT_EQ(observed[3].payload, Bytes({4}));
}

TEST(BufferLateJoinTest, DurableLateJoinDoesNotLoseDistinctKeys) {
  LateJoinFixture fixture;
  fixture.Start();
  for (const auto key : {"a", "b", "c"}) {
    ASSERT_TRUE(fixture.transport
                    .Put("sitos/buffers/session/durable/" + std::string(key), Bytes({1}),
                         {"zenoh/bytes"}, {})
                    .IsOk());
  }

  std::vector<Observation> observed;
  LateJoinCollector collector(
      fixture.transport, "sitos/buffers/session/durable/**",
      [&](const Observation& observation) { observed.push_back(observation); });
  ASSERT_TRUE(collector.Join());
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/a", Bytes({1}), {"zenoh/bytes"}, {})
          .IsOk());
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/b", Bytes({2}), {"zenoh/bytes"}, {})
          .IsOk());
  EXPECT_TRUE(collector.Failed());
  ASSERT_EQ(observed.size(), 3u);
  EXPECT_EQ(observed[0].payload, Bytes({1}));
  EXPECT_EQ(observed[1].payload, Bytes({1}));
  EXPECT_EQ(observed[2].payload, Bytes({1}));
}

TEST(BufferLateJoinTest, FailureInvokesNoObserverAndCleansUp) {
  LateJoinFixture fixture;
  fixture.Start();
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/a", Bytes({1}), {"zenoh/bytes"}, {})
          .IsOk());
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/b", Bytes({2}), {"zenoh/bytes"}, {})
          .IsOk());

  std::vector<Observation> observed;
  fixture.transport.declaration_failure = true;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation& observation) { observed.push_back(observation); });
    EXPECT_FALSE(collector.Join());
    EXPECT_TRUE(collector.Failed());
  }
  fixture.transport.declaration_failure = false;
  fixture.transport.fail_after_replies = 1;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation& observation) { observed.push_back(observation); });
    EXPECT_FALSE(collector.Join());
    EXPECT_TRUE(collector.Failed());
  }
  fixture.transport.fail_after_replies = -1;
  ASSERT_TRUE(
      fixture.transport
          .Put("sitos/buffers/session/durable/after-destroy", Bytes({9}), {"zenoh/bytes"}, {})
          .IsOk());
  EXPECT_TRUE(observed.empty());
}

}  // namespace
