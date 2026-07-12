// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode queryable integration tests using one zenoh session. zenoh-c
// 1.9.0 has a known same-process multi-session runtime failure; deterministic
// routing coverage is provided by the fake-transport unit tests.

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

class StorageNodeQueryIntegrationTest : public ::testing::Test {
 protected:
  struct ReplyState {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> keys;
    std::vector<std::byte> payload;
    std::string encoding;
  };

  void SetUp() override {
    transport_ = sitos::MakeZenohTransport();
    ASSERT_TRUE(transport_);
    engine_ = std::make_shared<sitos::InMemoryEngine>();
    node_ = std::make_unique<sitos::StorageNode>();
    ASSERT_TRUE(node_->Start(engine_, *transport_, {.prefix = "sitos/storage_node_test"}).IsOk());
  }

  void TearDown() override {
    node_.reset();
    transport_.reset();
  }

  std::unique_ptr<sitos::Transport> transport_;
  std::shared_ptr<sitos::InMemoryEngine> engine_;
  std::unique_ptr<sitos::StorageNode> node_;
};

TEST_F(StorageNodeQueryIntegrationTest, ExactGetRoundTrip) {
  const std::vector<std::byte> expected = {std::byte{0x00}, std::byte{0xA5}};
  ASSERT_TRUE(engine_->Put("foo/bar", expected));

  auto state = std::make_shared<ReplyState>();
  auto result = transport_->Get(
      "sitos/storage_node_test/base/foo/bar",
      [state](std::string_view key, std::span<const std::byte> payload,
              const sitos::Encoding& encoding) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->keys.emplace_back(key);
          state->payload.assign(payload.begin(), payload.end());
          state->encoding = encoding.id;
        }
        state->condition.notify_all();
        return false;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, std::chrono::seconds(3),
                                          [&state] { return state->keys.size() == 1; }));
  }
  EXPECT_EQ(state->keys[0], "sitos/storage_node_test/base/foo/bar");
  EXPECT_EQ(state->payload, expected);
  EXPECT_EQ(state->encoding, sitos::Encoding::kSitosV1);
}

TEST_F(StorageNodeQueryIntegrationTest, ChunkBoundaryListRoundTrip) {
  ASSERT_TRUE(engine_->Put("foo/bar", std::vector<std::byte>{std::byte{0x01}}));
  ASSERT_TRUE(engine_->Put("foobar/baz", std::vector<std::byte>{std::byte{0x02}}));

  auto state = std::make_shared<ReplyState>();
  auto result = transport_->Get(
      "sitos/storage_node_test/base/foo/**",
      [state](std::string_view key, std::span<const std::byte>,
              const sitos::Encoding& encoding) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->keys.emplace_back(key);
          state->encoding = encoding.id;
        }
        state->condition.notify_all();
        return true;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, std::chrono::seconds(3),
                                          [&state] { return state->keys.size() == 1; }));
  }
  ASSERT_EQ(state->keys.size(), 1u);
  EXPECT_EQ(state->keys[0], "sitos/storage_node_test/base/foo/bar");
  EXPECT_EQ(state->encoding, sitos::Encoding::kSitosV1);
}

TEST_F(StorageNodeQueryIntegrationTest, UnknownKeyProducesNoReply) {
  auto state = std::make_shared<ReplyState>();
  auto result = transport_->Get(
      "sitos/storage_node_test/base/missing",
      [state](std::string_view key, std::span<const std::byte>, const sitos::Encoding& encoding) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->keys.emplace_back(key);
          state->encoding = encoding.id;
        }
        state->condition.notify_all();
        return true;
      },
      std::chrono::milliseconds(500));

  ASSERT_TRUE(result.IsOk());
  std::unique_lock<std::mutex> lock(state->mutex);
  EXPECT_FALSE(state->condition.wait_for(lock, std::chrono::milliseconds(700),
                                         [&state] { return !state->keys.empty(); }));
}

}  // namespace
