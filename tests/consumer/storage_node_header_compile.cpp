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

int main() { return 0; }
