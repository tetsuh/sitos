// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Per-request completion state shared by Transport::Get implementations.

#ifndef SITOS_TRANSPORT_GET_COMPLETION_HPP
#define SITOS_TRANSPORT_GET_COMPLETION_HPP

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>

#include "sitos/transport.hpp"

namespace sitos::transport_internal {

/// A converted reply whose views remain valid only for ProcessReply().
struct QueryReply {
  std::string key;
  std::span<const std::byte> payload;
  Encoding encoding;
  std::shared_ptr<void> payload_keepalive;
};

/// Shared completion and delivery state for exactly one Transport::Get request.
class GetCompletion : public std::enable_shared_from_this<GetCompletion> {
 public:
  using ReplyConverter = std::function<Result<QueryReply>()>;

  class CallbackLease {
   public:
    CallbackLease(CallbackLease&& other) noexcept;
    CallbackLease& operator=(CallbackLease&& other) noexcept;
    ~CallbackLease();

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

   private:
    friend class GetCompletion;
    explicit CallbackLease(std::shared_ptr<GetCompletion> completion)
        : completion_(std::move(completion)) {}

    void Release() noexcept;

    std::shared_ptr<GetCompletion> completion_;
  };

  explicit GetCompletion(Transport::QueryResultSink sink);

  /// Enrolls the complete C callback before reply inspection or delivery waits.
  CallbackLease AcquireCallbackLease();

  /// Runs conversion and, if successful, delivery under the per-request gate.
  /// Conversion and sink failures become a terminal request error.
  void ProcessReply(const ReplyConverter& converter) noexcept;

  /// Marks native reply-closure drop. It is idempotent for submission failures.
  void MarkDropped() noexcept;

  /// Records an exception that escaped work surrounding ProcessReply().
  void RecordCallbackFailure() noexcept;

  /// Waits for terminal closure drop and every callback that already entered.
  Result<void> WaitForResult();

 private:
  enum class DeliveryState { kActive, kStoppedBySink, kFailed };

  void ReleaseCallback() noexcept;
  void RecordFailure(const ErrorInfo& error) noexcept;
  void RecordUnexpectedFailure() noexcept;
  bool IsActive() const;

  Transport::QueryResultSink sink_;
  mutable std::mutex state_mutex_;
  std::condition_variable state_condition_;
  bool dropped_ = false;
  std::size_t in_flight_ = 0;
  DeliveryState delivery_state_ = DeliveryState::kActive;
  std::optional<ErrorInfo> failure_;
  std::unordered_set<std::string> delivered_keys_;
  std::mutex delivery_mutex_;
};

}  // namespace sitos::transport_internal

#endif  // SITOS_TRANSPORT_GET_COMPLETION_HPP
