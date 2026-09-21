// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "sitos/buffer_publisher.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

std::filesystem::path UniqueRoot() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("sitos-buffer-publisher-rocksdb-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root);
  return root;
}

TEST(BufferPublisherRocksDbIntegrationTest, SyncedFenceSurvivesCloseAndReopen) {
  const auto root = UniqueRoot();
  const std::string prefix =
      "sitos/buffer-publisher-rocksdb-" + std::to_string(std::random_device{}());
  auto transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(transport_result.IsOk());
  auto transport = std::move(transport_result).Value();
  sitos::StorageNode node(*transport);
  ASSERT_TRUE(
      node.Start(
              std::make_shared<sitos::InMemoryEngine>(),
              {.prefix = prefix,
               .durable_buffer_engine_factory =
                   [root](std::string_view sid) {
                     auto opened = sitos::RocksDBEngine::Open((root / std::string(sid)).string());
                     if (!opened.IsOk()) {
                       return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
                     }
                     return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                         std::move(opened).Value());
                   }})
          .IsOk());
  ASSERT_TRUE(node.CreateSession("sid", {.durable_buffers = true}).IsOk());

  auto publisher_transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(publisher_transport_result.IsOk());
  std::shared_ptr<sitos::Transport> publisher_transport(
      std::move(publisher_transport_result).Value());
  sitos::ClientConfig config;
  config.prefix = prefix;
  config.query_timeout = std::chrono::seconds{5};
  auto opened =
      sitos::BufferPublisher::Open(publisher_transport, config, "sid", sitos::BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  auto publisher = std::move(opened).Value();
  ASSERT_TRUE(publisher.Push("before", std::vector<std::byte>{std::byte{1}}).IsOk());
  ASSERT_TRUE(publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{5}).IsOk());
  ASSERT_TRUE(publisher.Fence(sitos::FenceDurability::kSynced, std::chrono::seconds{5}).IsOk());
  ASSERT_TRUE(node.CloseSession("sid").IsOk());

  std::vector<std::byte> before_value;
  {
    auto first_persisted = sitos::RocksDBEngine::Open((root / "sid").string());
    ASSERT_TRUE(first_persisted.IsOk());
    auto first_engine = std::move(first_persisted).Value();
    ASSERT_TRUE(first_engine->Get("before", [&](std::string_view, sitos::Bytes bytes) {
      before_value.assign(bytes.begin(), bytes.end());
      return true;
    }));
  }
  EXPECT_EQ(before_value, (std::vector<std::byte>{std::byte{1}}));
  EXPECT_TRUE(publisher.Push("closed", std::vector<std::byte>{std::byte{2}}).IsOk());
  ASSERT_TRUE(node.CreateSession("sid", {.durable_buffers = true}).IsOk());
  const auto old_fence =
      publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::milliseconds{1500});
  EXPECT_EQ(old_fence.StatusCode(), sitos::Status::Timeout);
  EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            sitos::Status::Disconnected);
  EXPECT_EQ(
      publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::milliseconds{50}).StatusCode(),
      sitos::Status::Disconnected);

  auto reopened =
      sitos::BufferPublisher::Open(publisher_transport, config, "sid", sitos::BufferClass::Durable);
  ASSERT_TRUE(reopened.IsOk()) << reopened.Message();
  auto replacement = std::move(reopened).Value();
  ASSERT_TRUE(replacement.Push("after", std::vector<std::byte>{std::byte{3}}).IsOk());
  ASSERT_TRUE(replacement.Fence(sitos::FenceDurability::kSynced, std::chrono::seconds{5}).IsOk());
  ASSERT_TRUE(node.CloseSession("sid").IsOk());

  std::vector<std::byte> value;
  {
    auto persisted = sitos::RocksDBEngine::Open((root / "sid").string());
    ASSERT_TRUE(persisted.IsOk());
    auto second_engine = std::move(persisted).Value();
    ASSERT_TRUE(second_engine->Get("after", [&](std::string_view, sitos::Bytes bytes) {
      value.assign(bytes.begin(), bytes.end());
      return true;
    }));
  }
  EXPECT_EQ(value, (std::vector<std::byte>{std::byte{3}}));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  EXPECT_FALSE(error);
}

}  // namespace
