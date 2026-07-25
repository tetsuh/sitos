// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

class ParamStoreSubscribeIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto transport_result = sitos::OpenZenohTransport();
    ASSERT_TRUE(transport_result.IsOk());
    transport_ = std::shared_ptr<sitos::Transport>(std::move(transport_result).Value());
    config_.prefix = "sitos/param_subscribe_integration";
    auto store_result = sitos::ParamStore::Open(transport_, config_);
    ASSERT_TRUE(store_result.IsOk());
    store_ = std::move(store_result).Value();
  }

  void TearDown() override {
    control_ = sitos::Subscription{};
    store_.reset();
    transport_.reset();
  }

  std::shared_ptr<sitos::Transport> transport_;
  sitos::ClientConfig config_;
  std::optional<sitos::ParamStore> store_;
  sitos::Subscription control_;
};

TEST_F(ParamStoreSubscribeIntegrationTest, ReceivesPutDeleteAndStopsAfterClose) {
  std::mutex mutex;
  std::condition_variable condition;
  int control_count = 0;
  int callback_count = 0;
  std::vector<sitos::ParamChange> changes;
  auto control_result = transport_->DeclareSubscriber(
      config_.prefix + "/base/**", [&](const sitos::TransportSample&) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++control_count;
        }
        condition.notify_all();
      });
  ASSERT_TRUE(control_result.IsOk());
  control_ = std::move(control_result).Value();

  auto subscription = store_->Subscribe("base", "", [&](const sitos::ParamChange& change) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++callback_count;
      changes.push_back(change);
    }
    condition.notify_all();
  });
  ASSERT_TRUE(subscription.IsOk());

  const auto key = sitos::BuildKey(config_.prefix, "base", "value");
  ASSERT_TRUE(key.has_value());
  const auto payload = sitos::ParamValue(true).Encode();
  ASSERT_TRUE(transport_->Put(*key, payload,
                              sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return control_count >= 1 && callback_count >= 1; }));
  }
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().kind, sitos::ParamChangeKind::kPut);
  EXPECT_EQ(changes.front().key, "value");

  const int callback_baseline = callback_count;
  const int control_baseline = control_count;
  subscription.Value().Close();
  ASSERT_TRUE(transport_->Delete(*key, {}).IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return control_count > control_baseline; }));
  }
  EXPECT_EQ(callback_count, callback_baseline);
}

TEST_F(ParamStoreSubscribeIntegrationTest, ExpandsCanonicalBatchInOrder) {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<sitos::ParamChange> changes;
  auto subscription = store_->Subscribe("base", "", [&](const sitos::ParamChange& change) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      changes.push_back(change);
    }
    condition.notify_all();
  });
  ASSERT_TRUE(subscription.IsOk());

  std::vector<sitos::BatchEntry> entries{
      {"one", sitos::ParamValue(std::int64_t{1})},
      {"two", sitos::ParamValue(std::int64_t{2})},
      {"one", sitos::ParamValue(std::int64_t{3})}};
  const auto key = sitos::BuildBatchKey(config_.prefix, "base");
  ASSERT_TRUE(key.has_value());
  const auto payload = sitos::EncodeBatch(entries);
  ASSERT_TRUE(transport_->Put(*key, payload,
                              sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)}, {})
                  .IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return changes.size() >= entries.size(); }));
  }
  ASSERT_EQ(changes.size(), entries.size());
  EXPECT_EQ(changes[0].key, "one");
  EXPECT_EQ(changes[1].key, "two");
  EXPECT_EQ(changes[2].key, "one");
}

}  // namespace
