// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
    {
      std::unique_lock lock(copied_callback_mutex);
      if (park_copied_callbacks) {
        copied_callbacks_entered = true;
        copied_callback_cv.notify_all();
        copied_callback_cv.wait(lock, [&] { return !park_copied_callbacks; });
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
          if (!sink(key, payload, std::move(encoding))) {
            return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
          }
          if (reply_count == 1) {
            std::unique_lock lock(reply_mutex);
            if (block_after_first_reply) {
              first_reply_entered = true;
              reply_cv.notify_all();
              reply_cv.wait(lock, [&] { return !block_after_first_reply; });
            }
          }
          return sitos::Result<void>::Ok();
        });
    query.keyexpr = std::string(keyexpr);
    if (queryable_) queryable_(query);
    {
      std::unique_lock lock(get_mutex);
      if (block_get) {
        get_entered = true;
        get_cv.notify_all();
        get_cv.wait(lock, [&] { return !block_get; });
      }
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
    int id = 0;
    {
      std::scoped_lock lock(subscriber_mutex_);
      id = next_subscriber_id++;
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

  void WaitForFirstReply() {
    std::unique_lock lock(reply_mutex);
    reply_cv.wait(lock, [&] { return first_reply_entered; });
  }

  void ReleaseFirstReply() {
    {
      std::scoped_lock lock(reply_mutex);
      block_after_first_reply = false;
    }
    reply_cv.notify_all();
  }

  void WaitForCopiedCallbacks() {
    std::unique_lock lock(copied_callback_mutex);
    copied_callback_cv.wait(lock, [&] { return copied_callbacks_entered; });
  }

  void ReleaseCopiedCallbacks() {
    {
      std::scoped_lock lock(copied_callback_mutex);
      park_copied_callbacks = false;
    }
    copied_callback_cv.notify_all();
  }

  bool query_failure = false;
  bool declaration_failure = false;
  int fail_after_replies = -1;
  bool block_get = false;
  bool get_entered = false;
  bool block_after_first_reply = false;
  bool first_reply_entered = false;
  bool park_copied_callbacks = false;
  bool copied_callbacks_entered = false;
  std::mutex get_mutex;
  std::condition_variable get_cv;
  std::mutex reply_mutex;
  std::condition_variable reply_cv;
  std::mutex copied_callback_mutex;
  std::condition_variable copied_callback_cv;

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
    auto callback_state = std::make_shared<CallbackState>();
    callback_state->handler = [this](const sitos::TransportSample& sample) { OnSample(sample); };
    auto declaration = transport_.DeclareSubscriber(
        selector_,
        [callback_state](const sitos::TransportSample& sample) { callback_state->Invoke(sample); });
    if (!declaration.IsOk()) {
      MarkFailed();
      return false;
    }
    {
      std::scoped_lock lock(collector_mutex_);
      callback_state_ = callback_state;
      subscription_ = std::move(declaration).Value();
    }

    const auto result = transport_.Get(
        selector_,
        [this](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          std::scoped_lock lock(collector_mutex_);
          Observation observation{
              std::string(key), {payload.begin(), payload.end()}, std::move(encoding.id)};
          return AddMaterialized(observation);
        },
        std::chrono::seconds(1));

    std::vector<Observation> batch;
    std::unique_lock observation_lock(observation_mutex_, std::defer_lock);
    {
      std::unique_lock lock(collector_mutex_);
      if (!result.IsOk() || phase_ == Phase::Failed) {
        lock.unlock();
        MarkFailed();
        return false;
      }
      observation_lock.lock();
      phase_ = Phase::Live;
      batch = std::move(materialized_);
      batch.insert(batch.end(), std::make_move_iterator(buffered_.begin()),
                   std::make_move_iterator(buffered_.end()));
      materialized_.clear();
      buffered_.clear();
    }
    try {
      Dispatch(std::move(batch), observation_lock);
    } catch (...) {
      observer_failed_.store(true, std::memory_order_release);
      if (observation_lock.owns_lock()) observation_lock.unlock();
      MarkFailed();
      return false;
    }
    return true;
  }

  bool Failed() const {
    std::scoped_lock lock(collector_mutex_);
    return phase_ == Phase::Failed;
  }

  void EnableLiveBoundarySignal() {
    std::scoped_lock lock(collector_mutex_);
    live_boundary_enabled_ = true;
    live_boundary_attempted_ = false;
  }

  bool WaitForLiveBoundaryAttempt(std::chrono::milliseconds timeout) {
    std::unique_lock lock(collector_mutex_);
    return live_boundary_cv_.wait_for(lock, timeout, [&] { return live_boundary_attempted_; });
  }

  ~LateJoinCollector() { MarkFailed(); }

 private:
  enum class Phase { Collecting, Live, Failed };
  enum class Source { Materialized, Buffered, Live };

  struct Seen {
    Observation observation;
    Source source;
  };

  struct CallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    bool active = true;
    size_t in_flight = 0;
    std::function<void(const sitos::TransportSample&)> handler;

    void Invoke(const sitos::TransportSample& sample) {
      {
        std::scoped_lock lock(mutex);
        if (!active) return;
        ++in_flight;
      }
      struct Admission {
        CallbackState& state;
        ~Admission() {
          std::scoped_lock lock(state.mutex);
          --state.in_flight;
          if (state.in_flight == 0) state.condition.notify_all();
        }
      } admission{*this};
      handler(sample);
    }

    void DeactivateAndDrain() {
      std::unique_lock lock(mutex);
      active = false;
      condition.wait(lock, [&] { return in_flight == 0; });
      handler = {};
    }
  };

  static bool Same(const Observation& lhs, const Observation& rhs) {
    return lhs.key == rhs.key && lhs.payload == rhs.payload && lhs.encoding == rhs.encoding;
  }

  bool AddMaterialized(const Observation& observation) {
    const auto it = seen_.find(observation.key);
    if (it == seen_.end()) {
      seen_.emplace(observation.key, Seen{observation, Source::Materialized});
      materialized_.push_back(observation);
      return true;
    }
    if (!Same(it->second.observation, observation)) {
      phase_ = Phase::Failed;
      return false;
    }
    if (it->second.source == Source::Buffered) {
      const auto buffered =
          std::find_if(buffered_.begin(), buffered_.end(),
                       [&](const Observation& item) { return item.key == observation.key; });
      if (buffered != buffered_.end()) buffered_.erase(buffered);
      it->second.source = Source::Materialized;
      materialized_.push_back(observation);
    }
    return true;
  }

  bool AddBuffered(const Observation& observation) {
    const auto it = seen_.find(observation.key);
    if (it != seen_.end()) {
      if (!Same(it->second.observation, observation)) phase_ = Phase::Failed;
      return Same(it->second.observation, observation);
    }
    seen_.emplace(observation.key, Seen{observation, Source::Buffered});
    buffered_.push_back(observation);
    return true;
  }

  void OnSample(const sitos::TransportSample& sample) {
    std::vector<Observation> dispatch;
    std::unique_lock observation_lock(observation_mutex_, std::defer_lock);
    {
      std::unique_lock lock(collector_mutex_);
      Observation observation{
          sample.key, {sample.payload.begin(), sample.payload.end()}, sample.encoding.id};
      if (phase_ == Phase::Failed) return;
      if (phase_ == Phase::Collecting) {
        AddBuffered(observation);
        return;
      }
      if (const auto it = seen_.find(observation.key); it != seen_.end()) {
        if (!Same(it->second.observation, observation)) phase_ = Phase::Failed;
        return;
      }
      AddLive(observation);
      dispatch.push_back(observation);
      if (live_boundary_enabled_) {
        live_boundary_attempted_ = true;
        live_boundary_cv_.notify_all();
      }
    }
    observation_lock.lock();
    if (observer_failed_.load(std::memory_order_acquire)) return;
    try {
      Dispatch(std::move(dispatch), observation_lock);
    } catch (...) {
      observer_failed_.store(true, std::memory_order_release);
      if (observation_lock.owns_lock()) observation_lock.unlock();
      MarkFailedPhase();
    }
  }

  bool AddLive(const Observation& observation) {
    const auto it = seen_.find(observation.key);
    if (it != seen_.end()) {
      if (!Same(it->second.observation, observation)) phase_ = Phase::Failed;
      return Same(it->second.observation, observation);
    }
    seen_.emplace(observation.key, Seen{observation, Source::Live});
    return true;
  }

  void Dispatch(std::vector<Observation> batch, std::unique_lock<std::mutex>& observation_lock) {
    for (const auto& observation : batch) observer_(observation);
    observation_lock.unlock();
  }

  void MarkFailedPhase() {
    std::scoped_lock lock(collector_mutex_);
    phase_ = Phase::Failed;
  }

  void MarkFailed() {
    std::optional<sitos::Subscription> subscription;
    std::shared_ptr<CallbackState> callback_state;
    {
      std::scoped_lock lock(collector_mutex_);
      phase_ = Phase::Failed;
      subscription = std::move(subscription_);
      callback_state = std::move(callback_state_);
      materialized_.clear();
      buffered_.clear();
    }
    subscription.reset();
    if (callback_state) callback_state->DeactivateAndDrain();
  }

  LateJoinTransport& transport_;
  std::string selector_;
  std::function<void(const Observation&)> observer_;
  std::atomic<bool> observer_failed_ = false;
  bool live_boundary_enabled_ = false;
  bool live_boundary_attempted_ = false;
  std::condition_variable live_boundary_cv_;
  mutable std::mutex collector_mutex_;
  std::mutex observation_mutex_;
  std::optional<sitos::Subscription> subscription_;
  std::shared_ptr<CallbackState> callback_state_;
  std::map<std::string, Seen> seen_;
  std::vector<Observation> materialized_;
  std::vector<Observation> buffered_;
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
  fixture.transport.block_after_first_reply = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForFirstReply();
  const auto buffered_result = fixture.transport.Put("sitos/buffers/session/durable/buffered",
                                                     Bytes({3}), {"zenoh/bytes"}, {});
  fixture.transport.ReleaseFirstReply();
  join.join();
  ASSERT_TRUE(buffered_result.IsOk());
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

TEST(BufferLateJoinTest, BufferedIdenticalReplyPromotesToMaterializedCategory) {
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
  fixture.transport.block_after_first_reply = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForFirstReply();
  const auto buffered = fixture.transport.Put("sitos/buffers/session/durable/second", Bytes({2}),
                                              {"zenoh/bytes"}, {});
  fixture.transport.ReleaseFirstReply();
  join.join();
  ASSERT_TRUE(buffered.IsOk());
  ASSERT_TRUE(joined.has_value());
  ASSERT_TRUE(*joined);
  ASSERT_EQ(observed.size(), 2u);
  EXPECT_EQ(observed[0].key, "sitos/buffers/session/durable/first");
  EXPECT_EQ(observed[1].key, "sitos/buffers/session/durable/second");
  EXPECT_EQ(observed[1].payload, Bytes({2}));
}

TEST(BufferLateJoinTest, BufferedConflictingReplyFailsBeforeObserverDelivery) {
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
  fixture.transport.block_after_first_reply = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForFirstReply();
  const auto buffered = fixture.transport.Put("sitos/buffers/session/durable/second", Bytes({9}),
                                              {"zenoh/bytes"}, {});
  fixture.transport.ReleaseFirstReply();
  join.join();
  ASSERT_TRUE(buffered.IsOk());
  ASSERT_TRUE(joined.has_value());
  EXPECT_FALSE(*joined);
  EXPECT_TRUE(collector.Failed());
  EXPECT_TRUE(observed.empty());
}

TEST(BufferLateJoinTest, ObservationBoundarySerializesInitialBatchAndLiveSample) {
  LateJoinFixture fixture;
  fixture.Start();
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/first", Bytes({1}), {"zenoh/bytes"}, {})
          .IsOk());
  ASSERT_TRUE(
      fixture.transport.Put("sitos/buffers/session/durable/second", Bytes({2}), {"zenoh/bytes"}, {})
          .IsOk());

  std::vector<Observation> observed;
  std::mutex observer_mutex;
  std::condition_variable observer_cv;
  bool first_observer_entered = false;
  bool release_first_observer = false;
  LateJoinCollector collector(fixture.transport, "sitos/buffers/session/durable/**",
                              [&](const Observation& observation) {
                                {
                                  std::scoped_lock lock(observer_mutex);
                                  observed.push_back(observation);
                                  if (observation.key == "sitos/buffers/session/durable/first") {
                                    first_observer_entered = true;
                                    observer_cv.notify_all();
                                  }
                                }
                                if (observation.key == "sitos/buffers/session/durable/first") {
                                  std::unique_lock lock(observer_mutex);
                                  observer_cv.wait(lock, [&] { return release_first_observer; });
                                }
                              });
  collector.EnableLiveBoundarySignal();
  fixture.transport.block_after_first_reply = true;
  std::optional<bool> joined;
  std::thread join([&] { joined = collector.Join(); });
  fixture.transport.WaitForFirstReply();
  const auto buffered = fixture.transport.Put("sitos/buffers/session/durable/buffered", Bytes({3}),
                                              {"zenoh/bytes"}, {});
  fixture.transport.ReleaseFirstReply();
  std::thread live;
  bool live_started = false;
  {
    std::unique_lock lock(observer_mutex);
    if (!observer_cv.wait_for(lock, std::chrono::seconds(5),
                              [&] { return first_observer_entered; })) {
      release_first_observer = true;
    } else {
      live = std::thread([&] {
        static_cast<void>(fixture.transport.Put("sitos/buffers/session/durable/live", Bytes({4}),
                                                {"zenoh/bytes"}, {}));
      });
      live_started = true;
    }
  }
  observer_cv.notify_all();
  const bool boundary_seen =
      live_started && collector.WaitForLiveBoundaryAttempt(std::chrono::seconds(5));
  {
    std::scoped_lock lock(observer_mutex);
    release_first_observer = true;
  }
  observer_cv.notify_all();
  fixture.transport.ReleaseFirstReply();
  join.join();
  if (live_started) live.join();
  ASSERT_TRUE(buffered.IsOk());
  ASSERT_TRUE(boundary_seen) << "live path did not reach observation boundary";
  ASSERT_TRUE(joined.has_value());
  ASSERT_TRUE(*joined);
  ASSERT_EQ(observed.size(), 4u);
  EXPECT_EQ(observed[0].key, "sitos/buffers/session/durable/first");
  EXPECT_EQ(observed[1].key, "sitos/buffers/session/durable/second");
  EXPECT_EQ(observed[2].key, "sitos/buffers/session/durable/buffered");
  EXPECT_EQ(observed[3].key, "sitos/buffers/session/durable/live");
}

TEST(BufferLateJoinTest, DurableLateJoinDoesNotLoseDistinctKeys) {
  LateJoinFixture fixture;
  fixture.Start();
  const std::map<std::string, std::vector<std::byte>> expected{
      {"sitos/buffers/session/durable/a", Bytes({1})},
      {"sitos/buffers/session/durable/b", Bytes({2})},
      {"sitos/buffers/session/durable/c", Bytes({3})}};
  for (const auto& [key, payload] : expected) {
    ASSERT_TRUE(fixture.transport.Put(key, payload, {"zenoh/bytes"}, {}).IsOk());
  }

  std::vector<Observation> observed;
  LateJoinCollector collector(
      fixture.transport, "sitos/buffers/session/durable/**",
      [&](const Observation& observation) { observed.push_back(observation); });
  ASSERT_TRUE(collector.Join());
  ASSERT_EQ(observed.size(), expected.size());
  std::map<std::string, Observation> actual;
  for (const auto& observation : observed) {
    ASSERT_TRUE(actual.emplace(observation.key, observation).second);
  }
  ASSERT_EQ(actual.size(), expected.size());
  for (const auto& [key, payload] : expected) {
    ASSERT_EQ(actual.at(key).payload, payload);
    EXPECT_EQ(actual.at(key).encoding, "zenoh/bytes");
  }

  const auto duplicate =
      fixture.transport.Put("sitos/buffers/session/durable/a", Bytes({1}), {"zenoh/bytes"}, {});
  EXPECT_TRUE(duplicate.IsOk());
  EXPECT_FALSE(collector.Failed());
  EXPECT_EQ(observed.size(), expected.size());

  const auto conflict =
      fixture.transport.Put("sitos/buffers/session/durable/b", Bytes({9}), {"zenoh/bytes"}, {});
  EXPECT_TRUE(conflict.IsOk());
  EXPECT_TRUE(collector.Failed());
  EXPECT_EQ(observed.size(), expected.size());
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
  fixture.transport.query_failure = true;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation& observation) { observed.push_back(observation); });
    EXPECT_FALSE(collector.Join());
    EXPECT_TRUE(collector.Failed());
  }
  fixture.transport.query_failure = false;
  fixture.transport.fail_after_replies = 1;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation& observation) { observed.push_back(observation); });
    EXPECT_FALSE(collector.Join());
    EXPECT_TRUE(collector.Failed());
  }
  fixture.transport.fail_after_replies = -1;

  bool initial_exception_propagated = false;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation&) { throw std::runtime_error("initial observer failure"); });
    bool joined = true;
    try {
      joined = collector.Join();
    } catch (...) {
      initial_exception_propagated = true;
    }
    EXPECT_FALSE(joined);
    EXPECT_TRUE(collector.Failed());
  }
  EXPECT_FALSE(initial_exception_propagated);

  int live_observer_calls = 0;
  {
    LateJoinCollector collector(fixture.transport, "sitos/buffers/session/durable/**",
                                [&](const Observation&) {
                                  ++live_observer_calls;
                                  if (live_observer_calls > 2)
                                    throw std::runtime_error("live observer failure");
                                });
    ASSERT_TRUE(collector.Join());
    const auto first_live = fixture.transport.Put("sitos/buffers/session/durable/live-failure",
                                                  Bytes({6}), {"zenoh/bytes"}, {});
    EXPECT_TRUE(first_live.IsOk());
    EXPECT_TRUE(collector.Failed());
    const int calls_after_failure = live_observer_calls;
    const auto second_live = fixture.transport.Put("sitos/buffers/session/durable/live-suppressed",
                                                   Bytes({7}), {"zenoh/bytes"}, {});
    EXPECT_TRUE(second_live.IsOk());
    EXPECT_EQ(live_observer_calls, calls_after_failure);
  }

  fixture.transport.park_copied_callbacks = true;
  std::thread publish;
  {
    LateJoinCollector collector(
        fixture.transport, "sitos/buffers/session/durable/**",
        [&](const Observation& observation) { observed.push_back(observation); });
    ASSERT_TRUE(collector.Join());
    observed.clear();
    publish = std::thread([&] {
      static_cast<void>(fixture.transport.Put("sitos/buffers/session/durable/copied", Bytes({8}),
                                              {"zenoh/bytes"}, {}));
    });
    fixture.transport.WaitForCopiedCallbacks();
  }
  fixture.transport.ReleaseCopiedCallbacks();
  publish.join();
  EXPECT_TRUE(observed.empty());

  const auto after_destroy = fixture.transport.Put("sitos/buffers/session/durable/after-destroy",
                                                   Bytes({9}), {"zenoh/bytes"}, {});
  EXPECT_TRUE(after_destroy.IsOk());
  EXPECT_TRUE(observed.empty());
}

}  // namespace
