// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_BUFFER_PUBLISHER_HPP
#define SITOS_BUFFER_PUBLISHER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "sitos/client_config.hpp"
#include "sitos/key.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos {

enum class FenceDurability { kApplied = 0, kSynced = 1 };

struct FenceReceipt {
  std::uint64_t through_publish_sequence = 0;
  FenceDurability durability = FenceDurability::kApplied;
};

/// Explicit publisher for immutable byte values in one Session buffer scope.
class BufferPublisher {
 public:
  static Result<BufferPublisher> Open(ClientConfig config, std::string_view session_id,
                                      BufferClass buffer_class);
  static Result<BufferPublisher> Open(std::shared_ptr<Transport> transport, ClientConfig config,
                                      std::string_view session_id, BufferClass buffer_class);

  BufferPublisher(const BufferPublisher&) = delete;
  BufferPublisher& operator=(const BufferPublisher&) = delete;
  BufferPublisher(BufferPublisher&& other) noexcept;
  BufferPublisher& operator=(BufferPublisher&& other) noexcept;
  ~BufferPublisher();

  /// Submit a borrowed byte span during the call. Caller memory may be released or reused once
  /// Push returns; no borrowed span is retained after the underlying Transport::Put returns.
  Result<void> Push(std::string_view key, std::span<const std::byte> value);
  Result<FenceReceipt> Fence(FenceDurability durability, std::chrono::milliseconds timeout);

 private:
  struct Impl;
  explicit BufferPublisher(std::unique_ptr<Impl> impl);
  static Result<BufferPublisher> OpenWithTransport(std::shared_ptr<Transport> transport,
                                                   ClientConfig config, std::string_view session_id,
                                                   BufferClass buffer_class);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_BUFFER_PUBLISHER_HPP
