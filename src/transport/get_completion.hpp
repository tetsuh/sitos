// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Per-request completion state shared by Transport::Get implementations.

#ifndef SITOS_TRANSPORT_GET_COMPLETION_HPP
#define SITOS_TRANSPORT_GET_COMPLETION_HPP

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

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
 private:
  enum class DeliveryState { kActive, kStoppedBySink, kFailed };
  enum class FailureKind { kNone, kReplyConversion, kCallbackException };

 public:
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
  template <typename Converter>
  void ProcessReply(Converter&& converter) noexcept {
    std::unique_lock<std::mutex> delivery_lock(delivery_mutex_);
    if (!IsActive()) return;

    try {
      auto converted = std::forward<Converter>(converter)();
      if (!converted.IsOk()) {
        RecordFailure(converted.StatusCode(), converted.Error(), FailureKind::kReplyConversion);
        return;
      }

      QueryReply reply = std::move(converted).Value();
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (delivery_state_ != DeliveryState::kActive) return;
        if (!delivered_keys_.insert(reply.key).second) return;
      }

      if (sink_ && !sink_(reply.key, reply.payload, reply.encoding)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (delivery_state_ == DeliveryState::kActive) {
          delivery_state_ = DeliveryState::kStoppedBySink;
        }
      }
    } catch (...) {
      RecordUnexpectedFailure();
    }
  }

  /// Marks native reply-closure drop. It is idempotent for submission failures.
  void MarkDropped() noexcept;

  /// Records an exception that escaped work surrounding ProcessReply().
  void RecordCallbackFailure() noexcept;

  /// Waits for terminal closure drop and every callback that already entered.
  Result<void> WaitForResult();

 private:
  void ReleaseCallback() noexcept;
  void RecordFailure(Status status, std::error_code cause, FailureKind kind) noexcept;
  void RecordUnexpectedFailure() noexcept;
  bool IsActive() const;

  Transport::QueryResultSink sink_;
  mutable std::mutex state_mutex_;
  std::condition_variable state_condition_;
  bool dropped_ = false;
  std::size_t in_flight_ = 0;
  DeliveryState delivery_state_ = DeliveryState::kActive;
  FailureKind failure_kind_ = FailureKind::kNone;
  Status failure_status_ = Status::Error;
  std::error_code failure_cause_;
  std::unordered_set<std::string> delivered_keys_;
  std::mutex delivery_mutex_;
};

}  // namespace sitos::transport_internal

#endif  // SITOS_TRANSPORT_GET_COMPLETION_HPP
