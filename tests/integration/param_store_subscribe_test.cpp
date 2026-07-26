// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST_F(ParamStoreSubscribeIntegrationTest, ReceivesPutUnknownBytesDeleteAndStopsAfterClose) {
  std::mutex mutex;
  std::condition_variable condition;
  int control_count = 0;
  int callback_count = 0;
  std::vector<std::string> control_keys;
  std::vector<sitos::ParamChange> changes;
  auto control_result = transport_->DeclareSubscriber(
      config_.prefix + "/base/**", [&](const sitos::TransportSample& sample) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++control_count;
          control_keys.emplace_back(sample.key);
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

  const auto value_key = sitos::BuildKey(config_.prefix, "base", "value");
  const auto opaque_key = sitos::BuildKey(config_.prefix, "base", "opaque");
  ASSERT_TRUE(value_key.has_value());
  ASSERT_TRUE(opaque_key.has_value());
  const auto payload = sitos::ParamValue(true).Encode();
  ASSERT_TRUE(transport_->Put(*value_key, payload,
                              sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return control_count >= 1 && callback_count >= 1; }));
  }

  const std::vector<std::byte> opaque_payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  ASSERT_TRUE(transport_->Put(*opaque_key, opaque_payload,
                              sitos::Encoding{"application/octet-stream"}, {})
                  .IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return control_count >= 2 && callback_count >= 2; }));
  }

  ASSERT_TRUE(transport_->Delete(*value_key, {}).IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return control_count >= 3 && callback_count >= 3; }));
  }

  std::vector<sitos::ParamChange> observed;
  int callback_baseline = 0;
  int control_baseline = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    observed = changes;
    callback_baseline = callback_count;
    control_baseline = control_count;
  }
  ASSERT_EQ(observed.size(), 3U);
  EXPECT_EQ(observed[0].kind, sitos::ParamChangeKind::kPut);
  EXPECT_EQ(observed[0].key, "value");
  EXPECT_EQ(observed[1].key, "opaque");
  ASSERT_TRUE(observed[1].value.has_value());
  const auto opaque_value = observed[1].value->As<std::vector<std::byte>>();
  ASSERT_TRUE(opaque_value.has_value());
  EXPECT_EQ(*opaque_value, opaque_payload);
  EXPECT_EQ(observed[2].kind, sitos::ParamChangeKind::kDelete);
  EXPECT_EQ(observed[2].key, "value");
  EXPECT_FALSE(observed[2].value.has_value());

  subscription.Value().Close();
  const auto post_close_key = sitos::BuildKey(config_.prefix, "base", "post-close");
  ASSERT_TRUE(post_close_key.has_value());
  ASSERT_TRUE(transport_->Put(*post_close_key, payload,
                              sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return std::find(control_keys.begin(), control_keys.end(), *post_close_key) !=
             control_keys.end();
    }));
    EXPECT_EQ(callback_count, callback_baseline);
  }
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
  std::vector<sitos::ParamChange> observed;
  {
    std::lock_guard<std::mutex> lock(mutex);
    observed = changes;
  }
  ASSERT_EQ(observed.size(), entries.size());
  EXPECT_EQ(observed[0].key, "one");
  EXPECT_EQ(observed[1].key, "two");
  EXPECT_EQ(observed[2].key, "one");
}

}  // namespace
