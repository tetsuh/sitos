// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sitos/buffer_publisher.hpp"

namespace sitos {
namespace {

TEST(BufferPublisherApiTest, ExposesFrozenApiAndDurabilityTypes) {
  static_assert(std::is_same_v<decltype(&BufferPublisher::Open),
                               Result<BufferPublisher> (*)(ClientConfig, std::string_view,
                                                           BufferClass)>);
  static_cast<void>(static_cast<Result<BufferPublisher> (*)(ClientConfig, std::string_view,
                                                              BufferClass)>(
      &BufferPublisher::Open));
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
  auto result = BufferPublisher::Open(ClientConfig{}, "sid", BufferClass::Ephemeral);
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
  static_cast<void>(bytes);
}

}  // namespace
}  // namespace sitos
