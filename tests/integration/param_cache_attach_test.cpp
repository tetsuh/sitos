// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "param_cache_test_access.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"

namespace {

using namespace std::chrono_literals;
using Access = sitos::param_cache_test_access::ParamCacheTestAccess;

std::shared_ptr<sitos::Transport> MakeTransport() {
  return std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
}

class ParamCacheIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = MakeTransport();
    ASSERT_TRUE(transport_);
    engine_ = std::make_shared<sitos::InMemoryEngine>();
    ASSERT_TRUE(node_.Start(engine_, *transport_, {.prefix = std::string(kPrefix)}).IsOk());
    sitos::ClientConfig config;
    config.prefix = std::string(kPrefix);
    config.query_timeout = 1000ms;
    auto store = sitos::ParamStore::Open(transport_, config);
    ASSERT_TRUE(store.IsOk());
    store_.emplace(std::move(store).Value());
    auto cache = sitos::ParamCache::Open(transport_, config);
    ASSERT_TRUE(cache.IsOk());
    cache_.emplace(std::move(cache).Value());
  }

  void TearDown() override {
    if (cache_.has_value()) cache_->Detach();
    node_.Stop();
    transport_.reset();
  }

  static constexpr std::string_view kPrefix = "sitos/param_cache_test";
  std::shared_ptr<sitos::Transport> transport_;
  std::shared_ptr<sitos::InMemoryEngine> engine_;
  sitos::StorageNode node_;
  std::optional<sitos::ParamStore> store_;
  std::optional<sitos::ParamCache> cache_;
};

TEST_F(ParamCacheIntegrationTest, AttachDoesNotMissConcurrentPut) {
  ASSERT_TRUE(store_->Put("base", "before", std::int64_t{1}).IsOk());
  ASSERT_TRUE(node_.CreateSession("s1").IsOk());

  std::thread attach([&] { ASSERT_TRUE(cache_->Attach("s1").IsOk()); });
  ASSERT_TRUE(store_->Put("session/s1", "during_attach", std::int64_t{7}).IsOk());
  attach.join();

  bool observed = false;
  for (int attempt = 0; attempt < 100 && !observed; ++attempt) {
    observed = Access::Get(*cache_, "during_attach").has_value();
    if (!observed) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(observed);
  EXPECT_EQ(Access::Get(*cache_, "during_attach")->As<std::int64_t>(), 7);
  const auto before = Access::Get(*cache_, "before");
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->As<std::int64_t>(), 1);
}

TEST_F(ParamCacheIntegrationTest, BaseBatchAndDeleteRoundTrip) {
  ASSERT_TRUE(store_->Put("base", "value", std::int64_t{1}).IsOk());
  ASSERT_TRUE(cache_->AttachBase().IsOk());
  for (int attempt = 0; attempt < 100 && !Access::Get(*cache_, "value"); ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(Access::Get(*cache_, "value").has_value());
  ASSERT_TRUE(store_->Delete("base", "value").IsOk());
  for (int attempt = 0; attempt < 100 && Access::Get(*cache_, "value"); ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_FALSE(Access::Get(*cache_, "value").has_value());
}

}  // namespace
