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
  SessionOptions options;
  EXPECT_FALSE(options.durable);
  EXPECT_FALSE(options.ephemeral);
  DurableBufferEngineFactory factory = [](std::string_view) {
    return Result<std::unique_ptr<StorageEngine>>::Err(
        std::make_error_code(std::errc::operation_not_supported));
  };
  EXPECT_TRUE(static_cast<bool>(factory));
}

}  // namespace
}  // namespace sitos
