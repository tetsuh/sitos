// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>

#include "sitos/result.hpp"
#include "sitos/storage_node.hpp"

using LegacyCreateSession = sitos::Result<void> (sitos::StorageNode::*)(std::string_view);
using CapabilityCreateSession = sitos::Result<void> (sitos::StorageNode::*)(std::string_view,
                                                                            sitos::SessionOptions);
static_assert(
    std::is_same_v<decltype(static_cast<LegacyCreateSession>(&sitos::StorageNode::CreateSession)),
                   LegacyCreateSession>);
static_assert(std::is_same_v<
              decltype(static_cast<CapabilityCreateSession>(&sitos::StorageNode::CreateSession)),
              CapabilityCreateSession>);
static_assert(
    std::is_same_v<
        sitos::DurableBufferEngineFactory,
        std::function<sitos::Result<std::unique_ptr<sitos::StorageEngine>>(std::string_view)>>);

constexpr sitos::SessionOptions kBufferOptions{.durable_buffers = true, .ephemeral_buffers = true};
static_assert(kBufferOptions.durable_buffers && kBufferOptions.ephemeral_buffers);

int main() {
  sitos::StorageNodeConfig default_config{};
  sitos::StorageNodeConfig prefix_config{.prefix = "example"};
  sitos::StorageNodeConfig two_field_config{.prefix = "example", .log_sink = nullptr};
  sitos::StorageNodeConfig full_config{
      .prefix = "example", .log_sink = nullptr, .durable_buffer_engine_factory = {}};
  return (default_config.prefix == "sitos" && prefix_config.prefix == "example" &&
          two_field_config.prefix == "example" && full_config.prefix == "example")
             ? 0
             : 1;
}
