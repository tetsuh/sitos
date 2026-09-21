// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "sitos/buffer_publisher.hpp"

int main() {
  using Open = sitos::Result<sitos::BufferPublisher> (*)(sitos::ClientConfig, std::string_view,
                                                          sitos::BufferClass);
  static_cast<void>(static_cast<Open>(&sitos::BufferPublisher::Open));
  const std::span<const std::byte> bytes;
  static_cast<void>(bytes);
  const auto durability = sitos::FenceDurability::kApplied;
  static_cast<void>(durability);
  return 0;
}
