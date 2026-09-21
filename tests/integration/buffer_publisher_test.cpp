// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/buffer_publisher.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

std::string UniquePrefix() {
  std::random_device random;
  return "sitos/buffer_publisher_" + std::to_string(random()) + "_" + std::to_string(random());
}

TEST(BufferPublisherZenohIntegrationTest, AppliedFenceAndEphemeralValidation) {
  auto node_transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(node_transport_result.IsOk());
  std::shared_ptr<sitos::Transport> node_transport(std::move(node_transport_result).Value());
  const auto prefix = UniquePrefix();
  sitos::StorageNode node(*node_transport);
  ASSERT_TRUE(node.Start(std::make_shared<sitos::InMemoryEngine>(),
                         sitos::StorageNodeConfig{
                             .prefix = prefix,
                             .log_sink = nullptr,
                             .durable_buffer_engine_factory =
                                 [](std::string_view) {
                                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                                       std::make_unique<sitos::InMemoryEngine>());
                                 }})
                  .IsOk());
  ASSERT_TRUE(node.CreateSession("s107", sitos::SessionOptions{.durable_buffers = true,
                                                               .ephemeral_buffers = true})
                  .IsOk());

  auto publisher_transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(publisher_transport_result.IsOk());
  std::shared_ptr<sitos::Transport> publisher_transport(
      std::move(publisher_transport_result).Value());
  sitos::ClientConfig config;
  config.prefix = prefix;
  config.query_timeout = std::chrono::seconds{2};
  auto opened = sitos::BufferPublisher::Open(publisher_transport, config, "s107",
                                             sitos::BufferClass::Ephemeral);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  auto publisher = std::move(opened).Value();
  const std::array<std::byte, 4> value{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  ASSERT_TRUE(publisher.Push("value", value).IsOk());
  const auto receipt = publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{5});
  ASSERT_TRUE(receipt.IsOk()) << receipt.Message();
  EXPECT_EQ(receipt.Value().through_publish_sequence, 1U);
  EXPECT_EQ(receipt.Value().durability, sitos::FenceDurability::kApplied);
  EXPECT_EQ(publisher.Fence(sitos::FenceDurability::kSynced, std::chrono::seconds{1}).StatusCode(),
            sitos::Status::InvalidArgument);
}

}  // namespace
