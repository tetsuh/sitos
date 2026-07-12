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

  std::vector<std::byte> received;
  std::string received_key;
  std::string received_encoding;
  auto result = transport_->Get(
      "sitos/storage_node_test/base/foo/bar",
      [&](std::string_view key, std::span<const std::byte> payload, const sitos::Encoding& encoding) {
        received_key = std::string(key);
        received.assign(payload.begin(), payload.end());
        received_encoding = encoding.id;
        return false;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  EXPECT_EQ(received_key, "sitos/storage_node_test/base/foo/bar");
  EXPECT_EQ(received, expected);
  EXPECT_EQ(received_encoding, sitos::Encoding::kSitosV1);
}

TEST_F(StorageNodeQueryIntegrationTest, ChunkBoundaryListRoundTrip) {
  ASSERT_TRUE(engine_->Put("foo/bar", std::vector<std::byte>{std::byte{0x01}}));
  ASSERT_TRUE(engine_->Put("foobar/baz", std::vector<std::byte>{std::byte{0x02}}));

  std::vector<std::string> received_keys;
  auto result = transport_->Get(
      "sitos/storage_node_test/base/foo/**",
      [&](std::string_view key, std::span<const std::byte>, const sitos::Encoding& encoding) {
        EXPECT_EQ(encoding.id, sitos::Encoding::kSitosV1);
        received_keys.emplace_back(key);
        return true;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  ASSERT_EQ(received_keys.size(), 1u);
  EXPECT_EQ(received_keys[0], "sitos/storage_node_test/base/foo/bar");
}

TEST_F(StorageNodeQueryIntegrationTest, UnknownKeyProducesNoReply) {
  bool called = false;
  auto result = transport_->Get(
      "sitos/storage_node_test/base/missing",
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&) {
        called = true;
        return true;
      },
      std::chrono::milliseconds(500));

  ASSERT_TRUE(result.IsOk());
  EXPECT_FALSE(called);
}

}  // namespace
