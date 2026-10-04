// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ParamCache disconnect and reconnect recovery over Zenoh (Issue #20, ADR-0037).
// One injected Transport serves the node, the store, and the cache, as docs/06 requires for
// same-process integration tests.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"

namespace {

using namespace std::chrono_literals;

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

class ReconnectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::shared_ptr<sitos::Transport>(sitos::MakeZenohTransport().release());
    ASSERT_TRUE(transport_);
    ASSERT_TRUE(transport_->SupportsLiveliness());
    StartNode();
    sitos::ClientConfig config;
    config.prefix = std::string(kPrefix);
    config.query_timeout = 1000ms;
    auto store = sitos::ParamStore::Open(transport_, config);
    ASSERT_TRUE(store.IsOk()) << store.Message();
    store_.emplace(std::move(store).Value());
    auto cache = sitos::ParamCache::Open(transport_, config);
    ASSERT_TRUE(cache.IsOk()) << cache.Message();
    cache_.emplace(std::move(cache).Value());
  }

  void TearDown() override {
    if (cache_.has_value()) cache_->Detach();
    cache_.reset();
    store_.reset();
    if (node_) node_->Stop();
    node_.reset();
    transport_.reset();
  }

  void StartNode() {
    node_ = std::make_unique<sitos::StorageNode>();
    ASSERT_TRUE(node_->Start(engine_, *transport_, {.prefix = std::string(kPrefix)}).IsOk());
  }

  // Base writes travel through the node's subscriber, so wait until the engine holds the key.
  void PutBase(std::string_view key, std::int64_t value) {
    ASSERT_TRUE(store_->Put("base", key, value).IsOk());
    ASSERT_TRUE(WaitUntil([&] {
      return engine_->Get(key, [](std::string_view, std::span<const std::byte>) { return true; });
    })) << key;
  }

  std::int64_t Read(std::string_view key) const {
    return cache_->GetOr<std::int64_t>(key, std::int64_t{-1}).Value();
  }

  static constexpr std::string_view kPrefix = "sitos/reconnect_test";
  std::shared_ptr<sitos::Transport> transport_;
  std::shared_ptr<sitos::InMemoryEngine> engine_ = std::make_shared<sitos::InMemoryEngine>();
  std::unique_ptr<sitos::StorageNode> node_;
  std::optional<sitos::ParamStore> store_;
  std::optional<sitos::ParamCache> cache_;
};

// Acceptance criteria 1 and 2.
TEST_F(ReconnectTest, CacheSurvivesAStorageNodeRestartAndConvergesToTheNodeState) {
  PutBase("inherited", 1);
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  // The token history may arrive after Attach's first read; the cache then binds by a rebuild.
  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  ASSERT_TRUE(store_->Put("session/s1", "overlay_only", std::int64_t{2}).IsOk());
  ASSERT_TRUE(WaitUntil([&] { return Read("overlay_only") == 2; }));
  ASSERT_EQ(Read("inherited"), 1);

  node_->Stop();
  node_.reset();

  // Stale window: the flag is set, and reads return the last-known values without blocking.
  ASSERT_TRUE(WaitUntil([&] { return cache_->IsStale(); }));
  EXPECT_EQ(Read("inherited"), 1);
  EXPECT_EQ(Read("overlay_only"), 2);
  EXPECT_TRUE(cache_->Contains("inherited").Value());

  StartNode();
  PutBase("post_restart", 3);
  EXPECT_TRUE(cache_->IsStale()) << "a returning node without the Session must not clear stale";
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());

  // The cache converges to the node-authoritative state of the re-created Session.
  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  EXPECT_EQ(Read("post_restart"), 3) << "a value only in the new snapshot is not visible";
  EXPECT_EQ(Read("inherited"), 1);
  EXPECT_EQ(Read("overlay_only"), -1) << "a value that existed only before the restart remains";

  // Post-restart puts are visible.
  ASSERT_TRUE(store_->Put("session/s1", "after_restart", std::int64_t{4}).IsOk());
  ASSERT_TRUE(WaitUntil([&] { return Read("after_restart") == 4; }));
  ASSERT_TRUE(cache_->Put("from_cache", std::int64_t{5}).IsOk());
  EXPECT_EQ(Read("from_cache"), 5);
}

TEST_F(ReconnectTest, ClosingAndRecreatingTheSessionOnALiveNodeRebuildsTheCache) {
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  ASSERT_TRUE(store_->Put("session/s1", "first_generation", std::int64_t{1}).IsOk());
  ASSERT_TRUE(WaitUntil([&] { return Read("first_generation") == 1; }));

  ASSERT_TRUE(node_->CloseSession("s1").IsOk());
  ASSERT_TRUE(WaitUntil([&] { return cache_->IsStale(); }));
  EXPECT_EQ(Read("first_generation"), 1);

  PutBase("second_generation", 2);
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());
  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  EXPECT_EQ(Read("second_generation"), 2);
  EXPECT_EQ(Read("first_generation"), -1);
}

TEST_F(ReconnectTest, AttachBeforeTheSessionExistsStartsStaleAndRecoversWhenItIsCreated) {
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  EXPECT_TRUE(cache_->IsStale());
  ASSERT_TRUE(cache_->Put("early", std::int64_t{1}).IsOk()) << "writes are unchanged while stale";

  PutBase("inherited", 7);
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());

  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  EXPECT_EQ(Read("inherited"), 7);
}

TEST_F(ReconnectTest, DetachClearsTheStaleFlagAndALaterAttachBindsTheSession) {
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  ASSERT_TRUE(cache_->IsStale());

  cache_->Detach();

  EXPECT_FALSE(cache_->IsStale());
  EXPECT_EQ(cache_->Contains("anything").StatusCode(), sitos::Status::InvalidArgument);

  PutBase("inherited", 3);
  ASSERT_TRUE(node_->CreateSession("s1").IsOk());
  ASSERT_TRUE(cache_->Attach("s1").IsOk());
  ASSERT_TRUE(WaitUntil([&] { return !cache_->IsStale(); }));
  EXPECT_EQ(Read("inherited"), 3);
}

}  // namespace
