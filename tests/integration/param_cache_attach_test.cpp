// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "param_cache_test_access.hpp"
#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"

namespace {

using namespace std::chrono_literals;
using Access = sitos::param_cache_test_access::ParamCacheTestAccess;

class TrackingTransport final : public sitos::Transport {
 public:
  explicit TrackingTransport(std::shared_ptr<sitos::Transport> inner)
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
    {
      std::unique_lock lock(mutex_);
      ++get_count_;
      get_condition_.notify_all();
      if (block_first_get_ && get_count_ == 1) {
        get_condition_.wait(lock, [this] { return release_first_get_; });
      }
    }
    return inner_->Get(keyexpr, sink, timeout);
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    auto wrapped = [this, callback = std::move(callback)](
                       const sitos::TransportSample& sample) {
      callback(sample);
      {
        std::lock_guard lock(mutex_);
        ++callback_count_;
        callback_condition_.notify_all();
      }
    };
    auto result = inner_->DeclareSubscriber(keyexpr, std::move(wrapped));
    {
      std::lock_guard lock(mutex_);
      subscriber_declared_ = result.IsOk();
      declaration_condition_.notify_all();
    }
    return result;
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view keyexpr,
      std::function<void(sitos::TransportQuery&)> callback) override {
    return inner_->DeclareQueryable(keyexpr, std::move(callback));
  }

  void EnableFirstGetBarrier() {
    std::lock_guard lock(mutex_);
    block_first_get_ = true;
  }

  void WaitForSubscriberAndGet() {
    std::unique_lock lock(mutex_);
    ASSERT_TRUE(declaration_condition_.wait_for(lock, 5s,
                                                [this] { return subscriber_declared_; }));
    ASSERT_TRUE(get_condition_.wait_for(lock, 5s, [this] { return get_count_ >= 1; }));
  }

  void ReleaseFirstGet() {
    std::lock_guard lock(mutex_);
    release_first_get_ = true;
    get_condition_.notify_all();
  }

  void WaitForCallbackAfter(std::size_t previous) {
    std::unique_lock lock(mutex_);
    ASSERT_TRUE(callback_condition_.wait_for(lock, 5s, [this, previous] {
      return callback_count_ > previous;
    }));
  }

  std::size_t CallbackCount() const {
    std::lock_guard lock(mutex_);
    return callback_count_;
  }

 private:
  std::shared_ptr<sitos::Transport> inner_;
  mutable std::mutex mutex_;
  std::condition_variable declaration_condition_;
  std::condition_variable get_condition_;
  std::condition_variable callback_condition_;
  bool subscriber_declared_ = false;
  bool block_first_get_ = false;
  bool release_first_get_ = false;
  std::size_t get_count_ = 0;
  std::size_t callback_count_ = 0;
};

class ParamCacheIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
    ASSERT_TRUE(transport_);
    tracking_ = std::make_shared<TrackingTransport>(transport_);
    engine_ = std::make_shared<sitos::InMemoryEngine>();
    ASSERT_TRUE(node_.Start(engine_, *transport_, {.prefix = std::string(kPrefix)}).IsOk());
    sitos::ClientConfig config;
    config.prefix = std::string(kPrefix);
    config.query_timeout = 1000ms;
    auto store = sitos::ParamStore::Open(transport_, config);
    ASSERT_TRUE(store.IsOk());
    store_.emplace(std::move(store).Value());
    auto cache = sitos::ParamCache::Open(tracking_, config);
    ASSERT_TRUE(cache.IsOk());
    cache_.emplace(std::move(cache).Value());
  }

  void TearDown() override {
    if (cache_.has_value()) cache_->Detach();
    node_.Stop();
    tracking_.reset();
    transport_.reset();
  }

  static constexpr std::string_view kPrefix = "sitos/param_cache_test";
  std::shared_ptr<sitos::Transport> transport_;
  std::shared_ptr<TrackingTransport> tracking_;
  std::shared_ptr<sitos::InMemoryEngine> engine_;
  sitos::StorageNode node_;
  std::optional<sitos::ParamStore> store_;
  std::optional<sitos::ParamCache> cache_;
};

TEST_F(ParamCacheIntegrationTest, AttachDoesNotMissConcurrentPut) {
  ASSERT_TRUE(store_->Put("base", "before", std::int64_t{1}).IsOk());
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());
  tracking_->EnableFirstGetBarrier();

  sitos::Result<void> attach_result = sitos::Result<void>::Err(sitos::Status::Error, "unset");
  std::thread attach([&] { attach_result = cache_->Attach("s1"); });
  tracking_->WaitForSubscriberAndGet();
  const auto previous = tracking_->CallbackCount();
  ASSERT_TRUE(store_->Put("session/s1", "during_attach", std::int64_t{7}).IsOk());
  tracking_->WaitForCallbackAfter(previous);
  tracking_->ReleaseFirstGet();
  attach.join();

  ASSERT_TRUE(attach_result.IsOk()) << attach_result.Message();
  const auto value = Access::Get(*cache_, "during_attach");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->As<std::int64_t>(), 7);
  const auto before = Access::Get(*cache_, "before");
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->As<std::int64_t>(), 1);
}

TEST_F(ParamCacheIntegrationTest, SnapshotOverlayAndSessionDeltaRoundTrip) {
  ASSERT_TRUE(store_->Put("base", "inherited", std::int64_t{1}).IsOk());
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());
  ASSERT_TRUE(store_->Put("session/s1", "inherited", std::int64_t{2}).IsOk());
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  ASSERT_EQ(Access::Get(*cache_, "inherited")->As<std::int64_t>(), 2);

  const auto previous = tracking_->CallbackCount();
  ASSERT_TRUE(store_->Put("session/s1", "delta", std::int64_t{3}).IsOk());
  tracking_->WaitForCallbackAfter(previous);
  ASSERT_TRUE(Access::Get(*cache_, "delta").has_value());
  EXPECT_EQ(Access::Get(*cache_, "delta")->As<std::int64_t>(), 3);
}

TEST_F(ParamCacheIntegrationTest, BatchDuringAndAfterAttachIsAppliedInOrder) {
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());
  tracking_->EnableFirstGetBarrier();
  sitos::Result<void> attach_result = sitos::Result<void>::Err(sitos::Status::Error, "unset");
  std::thread attach([&] { attach_result = cache_->Attach("s1"); });
  tracking_->WaitForSubscriberAndGet();
  const auto during = std::vector<sitos::BatchEntry>{{"during_a", sitos::ParamValue(1)},
                                                     {"during_b", sitos::ParamValue(2)}};
  const auto previous = tracking_->CallbackCount();
  ASSERT_TRUE(store_->PutBatch("session/s1", during).IsOk());
  tracking_->WaitForCallbackAfter(previous);
  tracking_->ReleaseFirstGet();
  attach.join();
  ASSERT_TRUE(attach_result.IsOk()) << attach_result.Message();
  EXPECT_EQ(Access::Get(*cache_, "during_a")->As<std::int64_t>(), 1);
  EXPECT_EQ(Access::Get(*cache_, "during_b")->As<std::int64_t>(), 2);

  const auto after = std::vector<sitos::BatchEntry>{{"after_a", sitos::ParamValue(4)},
                                                     {"after_b", sitos::ParamValue(5)}};
  const auto after_previous = tracking_->CallbackCount();
  ASSERT_TRUE(store_->PutBatch("session/s1", after).IsOk());
  tracking_->WaitForCallbackAfter(after_previous);
  EXPECT_EQ(Access::Get(*cache_, "after_a")->As<std::int64_t>(), 4);
  EXPECT_EQ(Access::Get(*cache_, "after_b")->As<std::int64_t>(), 5);
}

TEST_F(ParamCacheIntegrationTest, DetachStopsFurtherUpdates) {
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  const auto previous = tracking_->CallbackCount();
  ASSERT_TRUE(store_->Put("session/s1", "before_detach", std::int64_t{1}).IsOk());
  tracking_->WaitForCallbackAfter(previous);
  ASSERT_TRUE(Access::Get(*cache_, "before_detach").has_value());
  cache_->Detach();
  ASSERT_TRUE(store_->Put("session/s1", "after_detach", std::int64_t{2}).IsOk());
  EXPECT_FALSE(Access::Get(*cache_, "after_detach").has_value());
}

}  // namespace
