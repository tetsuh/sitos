// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "get_completion.hpp"

#include <cassert>
#include <utility>

namespace sitos::transport_internal {

GetCompletion::CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : completion_(std::move(other.completion_)), enrolled_(other.enrolled_) {
  other.enrolled_ = false;
}

GetCompletion::CallbackLease& GetCompletion::CallbackLease::operator=(
    CallbackLease&& other) noexcept {
  if (this != &other) {
    Release();
    completion_ = std::move(other.completion_);
    enrolled_ = other.enrolled_;
    other.enrolled_ = false;
  }
  return *this;
}

GetCompletion::CallbackLease::~CallbackLease() { Release(); }

void GetCompletion::CallbackLease::Release() noexcept {
  if (completion_ && enrolled_) {
    completion_->ReleaseCallback();
  }
  enrolled_ = false;
  completion_.reset();
}

GetCompletion::GetCompletion(Transport::QueryResultSink sink) : sink_(std::move(sink)) {}

GetCompletion::CallbackLease GetCompletion::AcquireCallbackLease(
    const std::shared_ptr<GetCompletion>* completion_context) noexcept {
  std::shared_ptr<GetCompletion> completion;
  bool enrolled = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    completion = *completion_context;
    if (!dropped_) {
      ++in_flight_;
      enrolled = true;
    }
  }
  return CallbackLease(std::move(completion), enrolled);
}

void GetCompletion::MarkDropped() noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    dropped_ = true;
  }
  state_condition_.notify_all();
}

void GetCompletion::RecordCallbackFailure() noexcept { RecordUnexpectedFailure(); }

Result<void> GetCompletion::WaitForResult() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  state_condition_.wait(lock, [this] { return dropped_ && in_flight_ == 0; });
  if (delivery_state_ == DeliveryState::kFailed) {
    const char* message = failure_kind_ == FailureKind::kReplyConversion
                              ? "failed to process zenoh get reply"
                              : "get reply callback failed";
    return Result<void>::Err(failure_status_, message, failure_cause_);
  }
  return Result<void>::Ok();
}

void GetCompletion::ReleaseCallback() noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    assert(in_flight_ > 0);
    if (in_flight_ > 0) --in_flight_;
  }
  state_condition_.notify_all();
}

void GetCompletion::RecordFailure(Status status, std::error_code cause, FailureKind kind) noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (delivery_state_ == DeliveryState::kActive) {
    delivery_state_ = DeliveryState::kFailed;
    failure_kind_ = kind;
    failure_status_ = status;
    failure_cause_ = cause ? cause : MakeErrorCode(status);
  }
}

void GetCompletion::RecordUnexpectedFailure() noexcept {
  RecordFailure(Status::Error, MakeErrorCode(Status::Error), FailureKind::kCallbackException);
}

bool GetCompletion::IsActive() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return delivery_state_ == DeliveryState::kActive;
}

}  // namespace sitos::transport_internal
