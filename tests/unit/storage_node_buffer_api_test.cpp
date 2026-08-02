// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <type_traits>

#include "sitos/storage_node.hpp"

namespace sitos {
namespace {

static_assert(std::is_same_v<decltype(static_cast<Result<void> (StorageNode::*)(std::string_view)>(
                                 &StorageNode::CreateSession)),
                             Result<void> (StorageNode::*)(std::string_view)>);
static_assert(std::is_same_v<
              decltype(static_cast<Result<void> (StorageNode::*)(std::string_view, SessionOptions)>(
                  &StorageNode::CreateSession)),
              Result<void> (StorageNode::*)(std::string_view, SessionOptions)>);
static_assert(
    std::is_same_v<DurableBufferEngineFactory,
                   std::function<Result<std::unique_ptr<StorageEngine>>(std::string_view)>>);

TEST(StorageNodeBufferApiTest, PreservesLegacyCreateSessionOverload) {
  auto member =
      static_cast<Result<void> (StorageNode::*)(std::string_view)>(&StorageNode::CreateSession);
  EXPECT_NE(member, nullptr);
}

TEST(StorageNodeBufferApiTest, ExposesCapabilityOverloadAndFactory) {
  SessionOptions options{.durable_buffers = true, .ephemeral_buffers = true};
  EXPECT_TRUE(options.durable_buffers);
  EXPECT_TRUE(options.ephemeral_buffers);
  StorageNodeConfig default_config{};
  StorageNodeConfig prefix_config{.prefix = "example"};
  StorageNodeConfig two_field_config{.prefix = "example", .log_sink = nullptr};
  EXPECT_EQ(default_config.prefix, "sitos");
  EXPECT_EQ(prefix_config.prefix, "example");
  EXPECT_EQ(two_field_config.prefix, "example");
  DurableBufferEngineFactory factory = [](std::string_view) {
    return Result<std::unique_ptr<StorageEngine>>::Err(
        std::make_error_code(std::errc::operation_not_supported));
  };
  EXPECT_TRUE(static_cast<bool>(factory));
}

}  // namespace
}  // namespace sitos
