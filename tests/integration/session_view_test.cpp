// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/session_view.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"

namespace {

using namespace std::chrono_literals;

class CallbackTrackingTransport final : public sitos::Transport {
 public:
  explicit CallbackTrackingTransport(std::shared_ptr<sitos::Transport> inner)
      : inner_(std::move(inner)) {}

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    return inner_->Put(key, payload, std::move(encoding), options);
  }

  sitos::Result<void> Delete(std::string_view key, sitos::PutOptions options) override {
    return inner_->Delete(key, options);
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    return inner_->Get(keyexpr, sink, timeout);
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    auto wrapped = [this, callback = std::move(callback)](
                       const sitos::TransportSample& sample) {
      callback(sample);
      std::lock_guard lock(mutex_);
      ++callback_count_;
      callback_cv_.notify_all();
    };
    return inner_->DeclareSubscriber(keyexpr, std::move(wrapped));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view keyexpr,
      std::function<void(sitos::TransportQuery&)> callback) override {
    return inner_->DeclareQueryable(keyexpr, std::move(callback));
  }

  std::size_t CallbackCount() const {
    std::lock_guard lock(mutex_);
    return callback_count_;
  }

  void WaitForCallbackAfter(std::size_t previous) {
    std::unique_lock lock(mutex_);
    ASSERT_TRUE(callback_cv_.wait_for(lock, 5s, [this, previous] {
      return callback_count_ > previous;
    }));
  }

 private:
  std::shared_ptr<sitos::Transport> inner_;
  mutable std::mutex mutex_;
  std::condition_variable callback_cv_;
  std::size_t callback_count_ = 0;
};

TEST(SessionViewIntegrationTest, MatchesParamCacheAfterObservedSessionDelivery) {
  auto transport = std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
  ASSERT_TRUE(transport);
  auto node_tracking = std::make_shared<CallbackTrackingTransport>(transport);
  auto cache_tracking = std::make_shared<CallbackTrackingTransport>(transport);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node;
  ASSERT_TRUE(node.Start(engine, *node_tracking, {.prefix = "sitos/session_view_test"}).IsOk());

  sitos::ClientConfig config;
  config.prefix = "sitos/session_view_test";
  config.query_timeout = 1000ms;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  const auto node_before_base = node_tracking->CallbackCount();
  ASSERT_TRUE(store.Put("base", "inherited", std::int64_t{1}).IsOk());
  node_tracking->WaitForCallbackAfter(node_before_base);
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  auto cache_result = sitos::ParamCache::Open(cache_tracking, config);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  auto view_result = sitos::SessionView::Open(node, "s1");
  ASSERT_TRUE(view_result.IsOk());
  auto view = std::move(view_result).Value();
  EXPECT_EQ(view.Get<std::int64_t>("inherited").Value(), 1);
  EXPECT_EQ(cache.Get<std::int64_t>("inherited").Value(), 1);

  const auto node_before_session = node_tracking->CallbackCount();
  const auto cache_before_session = cache_tracking->CallbackCount();
  ASSERT_TRUE(store.Put("session/s1", "result", std::int64_t{42}).IsOk());
  node_tracking->WaitForCallbackAfter(node_before_session);
  cache_tracking->WaitForCallbackAfter(cache_before_session);
  ASSERT_EQ(view.Get<std::int64_t>("result").Value(), 42);
  ASSERT_EQ(cache.Get<std::int64_t>("result").Value(), 42);

  cache.Detach();
  node.Stop();
}

}  // namespace
