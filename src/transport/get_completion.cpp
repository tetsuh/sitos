// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "get_completion.hpp"

#include <cassert>
#include <utility>

namespace sitos::transport_internal {

GetCompletion::CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : completion_(std::move(other.completion_)) {}

GetCompletion::CallbackLease& GetCompletion::CallbackLease::operator=(
    CallbackLease&& other) noexcept {
  if (this != &other) {
    Release();
    completion_ = std::move(other.completion_);
  }
  return *this;
}

GetCompletion::CallbackLease::~CallbackLease() { Release(); }

void GetCompletion::CallbackLease::Release() noexcept {
  if (completion_) {
    completion_->ReleaseCallback();
    completion_.reset();
  }
}

GetCompletion::GetCompletion(Transport::QueryResultSink sink) : sink_(std::move(sink)) {}

GetCompletion::CallbackLease GetCompletion::AcquireCallbackLease() {
  auto completion = shared_from_this();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++in_flight_;
  }
  return CallbackLease(std::move(completion));
}

void GetCompletion::ProcessReply(const ReplyConverter& converter) noexcept {
  std::unique_lock<std::mutex> delivery_lock(delivery_mutex_);
  if (!IsActive()) return;

  try {
    auto converted = converter();
    if (!converted.IsOk()) {
      RecordFailure(
          ErrorInfo{converted.StatusCode(), std::string(converted.Message()), converted.Error()});
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
  if (failure_.has_value()) {
    return Result<void>::Err(failure_->status, failure_->message, failure_->cause);
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

void GetCompletion::RecordFailure(const ErrorInfo& error) noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (delivery_state_ == DeliveryState::kActive) {
    delivery_state_ = DeliveryState::kFailed;
    failure_ = error;
  }
}

void GetCompletion::RecordUnexpectedFailure() noexcept {
  RecordFailure(
      ErrorInfo{Status::Error, "get reply callback failed", MakeErrorCode(Status::Error)});
}

bool GetCompletion::IsActive() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return delivery_state_ == DeliveryState::kActive;
}

}  // namespace sitos::transport_internal
