// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ParamStore round-trip coverage using one shared same-process zenoh transport.

#include "sitos/param_store.hpp"

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
#include <utility>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

using namespace std::chrono_literals;

std::shared_ptr<sitos::Transport> MakeSharedZenohTransport() {
  return std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
}

std::optional<sitos::ParamValue> GetEventually(sitos::ParamStore& store, std::string_view scope,
                                               std::string_view key) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto result = store.Get(scope, key);
    if (result.IsOk()) return std::move(result).Value();
    if (result.StatusCode() != sitos::Status::NotFound) return std::nullopt;
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

class ParamStoreIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = MakeSharedZenohTransport();
    ASSERT_TRUE(transport_);
    engine_ = std::make_shared<sitos::InMemoryEngine>();
    ASSERT_TRUE(node_.Start(engine_, *transport_, {.prefix = std::string(kPrefix)}).IsOk());

    sitos::ClientConfig config;
    config.prefix = std::string(kPrefix);
    config.query_timeout = 500ms;
    auto store_result = sitos::ParamStore::Open(transport_, std::move(config));
    ASSERT_TRUE(store_result.IsOk());
    store_.emplace(std::move(store_result).Value());
  }

  void TearDown() override {
    node_.Stop();
    transport_.reset();
  }

  static constexpr std::string_view kPrefix = "sitos/param_store_test";
  std::shared_ptr<sitos::Transport> transport_;
  std::shared_ptr<sitos::InMemoryEngine> engine_;
  sitos::StorageNode node_;
  std::optional<sitos::ParamStore> store_;
};

TEST_F(ParamStoreIntegrationTest, BasePutGetListAndDeleteRoundTrip) {
  ASSERT_TRUE(store_->Put("base", "foo/bar", std::int64_t{7}).IsOk());
  ASSERT_TRUE(store_->Put("base", "foobar", std::string("outside")).IsOk());

  auto value = GetEventually(*store_, "base", "foo/bar");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->As<std::int64_t>(), 7);

  std::vector<std::string> keys;
  ASSERT_TRUE(store_
                  ->List("base", "foo",
                         [&](std::string_view key, const sitos::ParamValue&) {
                           keys.emplace_back(key);
                           return true;
                         })
                  .IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/bar", "foobar"}));

  keys.clear();
  ASSERT_TRUE(store_
                  ->List("base", "foo/",
                         [&](std::string_view key, const sitos::ParamValue&) {
                           keys.emplace_back(key);
                           return true;
                         })
                  .IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/bar"}));

  ASSERT_TRUE(store_->Delete("base", "foo/bar").IsOk());
  EXPECT_FALSE(GetEventually(*store_, "base", "foo/bar").has_value());
}

TEST_F(ParamStoreIntegrationTest, SessionOverlayAndSnapshotRoundTrip) {
  ASSERT_TRUE(store_->Put("base", "before", std::int64_t{1}).IsOk());
  auto before = GetEventually(*store_, "base", "before");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(node_.CreateSession("s1").IsOk());
  ASSERT_TRUE(store_->Put("base", "after", std::int64_t{2}).IsOk());
  ASSERT_TRUE(store_->Put("session/s1", "overlay", std::int64_t{3}).IsOk());

  auto snapshot_before = GetEventually(*store_, "snap/s1", "before");
  ASSERT_TRUE(snapshot_before.has_value());
  EXPECT_EQ(snapshot_before->As<std::int64_t>(), 1);
  EXPECT_FALSE(GetEventually(*store_, "snap/s1", "after").has_value());

  auto overlay = GetEventually(*store_, "session/s1", "overlay");
  ASSERT_TRUE(overlay.has_value());
  EXPECT_EQ(overlay->As<std::int64_t>(), 3);
}

TEST_F(ParamStoreIntegrationTest, PutBatchSendsOneCanonicalMessageAndAppliesInOrder) {
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());

  struct Observation {
    std::mutex mutex;
    std::condition_variable condition;
    int count = 0;
    std::string key;
    std::vector<std::byte> payload;
    std::string encoding;
  };
  auto observation = std::make_shared<Observation>();
  auto subscriber_result = transport_->DeclareSubscriber(
      std::string(kPrefix) + "/session/s1/**", [observation](const sitos::TransportSample& sample) {
        std::lock_guard lock(observation->mutex);
        ++observation->count;
        observation->key = sample.key;
        observation->payload.assign(sample.payload.begin(), sample.payload.end());
        observation->encoding = sample.encoding.id;
        observation->condition.notify_all();
      });
  ASSERT_TRUE(subscriber_result.IsOk());
  auto subscriber = std::move(subscriber_result).Value();

  const std::vector<sitos::BatchEntry> entries = {
      {"first", sitos::ParamValue(std::int64_t{1})},
      {"first", sitos::ParamValue(std::int64_t{2})},
      {"second", sitos::ParamValue("two")},
  };
  ASSERT_TRUE(store_->PutBatch("session/s1", entries).IsOk());

  {
    std::unique_lock lock(observation->mutex);
    ASSERT_TRUE(observation->condition.wait_for(lock, 3s, [&] { return observation->count == 1; }));
    EXPECT_EQ(observation->key, std::string(kPrefix) + "/session/s1/:batch");
    EXPECT_EQ(observation->encoding, sitos::Encoding::kSitosV1Batch);
  }
  auto decoded = sitos::DecodeBatch(observation->payload);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), entries.size());
  EXPECT_EQ((*decoded)[0].key, "first");
  EXPECT_EQ((*decoded)[1].value.As<std::int64_t>(), 2);
  EXPECT_EQ((*decoded)[2].key, "second");

  auto first = GetEventually(*store_, "session/s1", "first");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->As<std::int64_t>(), 2);
  auto second = GetEventually(*store_, "session/s1", "second");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->As<std::string>(), "two");
}

TEST(ParamStoreFactoryIntegrationTest, DefaultOpenUsesConfigAndFactory) {
  sitos::ClientConfig config;
  config.prefix = "sitos/param_store_factory_test";
  config.query_timeout = 500ms;
  auto result = sitos::ParamStore::Open(std::move(config));
  ASSERT_TRUE(result.IsOk()) << result.Message();
}

}  // namespace
