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
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

std::string UniquePrefix() {
  std::random_device random;
  return "sitos/buffer_publisher_" + std::to_string(random()) + "_" + std::to_string(random());
}

TEST(BufferPublisherZenohIntegrationTest, SameSidRecreationRejectsOldFenceAndAcceptsReplacement) {
  auto node_transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(node_transport_result.IsOk());
  std::shared_ptr<sitos::Transport> node_transport(std::move(node_transport_result).Value());
  const auto prefix = UniquePrefix();
  sitos::StorageNode node(*node_transport);
  ASSERT_TRUE(node.Start(std::make_shared<sitos::InMemoryEngine>(),
                         {.prefix = prefix,
                          .durable_buffer_engine_factory =
                              [](std::string_view) {
                                return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                                    std::make_unique<sitos::InMemoryEngine>());
                              }})
                  .IsOk());
  ASSERT_TRUE(node.CreateSession("sid", sitos::SessionOptions{.durable_buffers = true}).IsOk());
  auto publisher_transport_result = sitos::OpenZenohTransport();
  ASSERT_TRUE(publisher_transport_result.IsOk());
  std::shared_ptr<sitos::Transport> publisher_transport(
      std::move(publisher_transport_result).Value());
  sitos::ClientConfig config;
  config.prefix = prefix;
  config.query_timeout = std::chrono::seconds{2};
  auto opened =
      sitos::BufferPublisher::Open(publisher_transport, config, "sid", sitos::BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  auto old_publisher = std::move(opened).Value();
  ASSERT_TRUE(node.CloseSession("sid").IsOk());
  ASSERT_TRUE(node.CreateSession("sid", sitos::SessionOptions{.durable_buffers = true}).IsOk());
  const auto old_fence =
      old_publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{2});
  EXPECT_FALSE(old_fence.IsOk());

  auto replacement_open =
      sitos::BufferPublisher::Open(publisher_transport, config, "sid", sitos::BufferClass::Durable);
  ASSERT_TRUE(replacement_open.IsOk()) << replacement_open.Message();
  auto replacement = std::move(replacement_open).Value();
  ASSERT_TRUE(replacement.Push("replacement", std::vector<std::byte>{std::byte{1}}).IsOk());
  EXPECT_TRUE(replacement.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{2}).IsOk());
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
  std::array<std::byte, 4> value{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  ASSERT_TRUE(publisher.Push("value", value).IsOk());
  value.fill(std::byte{0xff});
  const auto receipt = publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{5});
  ASSERT_TRUE(receipt.IsOk()) << receipt.Message();
  EXPECT_EQ(receipt.Value().through_publish_sequence, 1U);
  EXPECT_EQ(receipt.Value().durability, sitos::FenceDurability::kApplied);
  EXPECT_EQ(publisher.Fence(sitos::FenceDurability::kSynced, std::chrono::seconds{1}).StatusCode(),
            sitos::Status::InvalidArgument);
  auto durable_open = sitos::BufferPublisher::Open(publisher_transport, config, "s107",
                                                   sitos::BufferClass::Durable);
  ASSERT_TRUE(durable_open.IsOk()) << durable_open.Message();
  auto durable_publisher = std::move(durable_open).Value();
  std::array<std::byte, 4> durable_value{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  const auto expected_value = durable_value;
  ASSERT_TRUE(durable_publisher.Push("value", durable_value).IsOk());
  durable_value.fill(std::byte{0xff});
  ASSERT_TRUE(
      durable_publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::seconds{5}).IsOk());
  std::vector<std::byte> observed;
  std::size_t replies = 0;
  ASSERT_TRUE(publisher_transport
                  ->Get(
                      prefix + "/buffers/s107/durable/value",
                      [&](std::string_view, std::span<const std::byte> bytes, sitos::Encoding) {
                        observed.assign(bytes.begin(), bytes.end());
                        ++replies;
                        return true;
                      },
                      std::chrono::seconds{2})
                  .IsOk());
  ASSERT_EQ(replies, 1U);
  EXPECT_EQ(observed, std::vector<std::byte>(expected_value.begin(), expected_value.end()));

  auto configured = config;
  configured.zenoh_config_json = R"({"mode":"peer"})";
  auto normal_open =
      sitos::BufferPublisher::Open(configured, "s107", sitos::BufferClass::Ephemeral);
  ASSERT_EQ(normal_open.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(normal_open.Message(), "Transport does not support Fence");
}

}  // namespace
