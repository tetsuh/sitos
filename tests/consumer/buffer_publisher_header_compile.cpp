// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "sitos/buffer_publisher.hpp"

int main() {
  using Open = sitos::Result<sitos::BufferPublisher> (*)(sitos::ClientConfig, std::string_view,
                                                         sitos::BufferClass);
  static_cast<void>(static_cast<Open>(&sitos::BufferPublisher::Open));

  auto opened =
      sitos::BufferPublisher::Open(sitos::ClientConfig{}, "session", sitos::BufferClass::Durable);
  if (opened.IsOk()) {
    auto publisher = std::move(opened).Value();
    std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto pushed = publisher.Push("key", std::span<const std::byte>(bytes));
    static_cast<void>(pushed);
    const auto fenced =
        publisher.Fence(sitos::FenceDurability::kApplied, std::chrono::milliseconds{1});
    if (fenced.IsOk()) {
      const sitos::FenceReceipt& receipt = fenced.Value();
      static_cast<void>(receipt.through_publish_sequence);
      static_cast<void>(receipt.durability);
    }
  }
  return 0;
}
