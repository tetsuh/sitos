// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Transport handle stubs used when zenoh support is disabled.

#include "sitos/transport.hpp"

#include <utility>

namespace sitos {

struct TransportQuery::Impl {};
struct Subscription::Impl {};
struct Queryable::Impl {};

TransportQuery::TransportQuery() = default;
TransportQuery::TransportQuery(ReplyHandler handler)
    : test_reply_handler_(std::move(handler)) {}
TransportQuery::~TransportQuery() = default;

Result<void> TransportQuery::Reply(std::string_view key, std::span<const std::byte> payload,
                                   Encoding encoding) {
  if (test_reply_handler_) return test_reply_handler_(key, payload, encoding);
  return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
}

Subscription::Subscription() = default;
Subscription::Subscription(std::function<void()> reset_handler)
    : reset_handler_(std::move(reset_handler)) {}
Subscription::~Subscription() { Reset(); }
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_ = std::move(other.impl_);
    reset_handler_ = std::move(other.reset_handler_);
  }
  return *this;
}

void Subscription::Reset() noexcept {
  impl_.reset();
  if (reset_handler_) {
    try {
      reset_handler_();
    } catch (...) {
    }
    reset_handler_ = {};
  }
}

Queryable::Queryable() = default;
Queryable::Queryable(std::function<void()> reset_handler)
    : reset_handler_(std::move(reset_handler)) {}
Queryable::~Queryable() { Reset(); }
void Queryable::Reset() noexcept {
  impl_.reset();
  if (reset_handler_) {
    try {
      reset_handler_();
    } catch (...) {
    }
    reset_handler_ = {};
  }
}
Queryable::Queryable(Queryable&&) noexcept = default;
Queryable& Queryable::operator=(Queryable&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_ = std::move(other.impl_);
    reset_handler_ = std::move(other.reset_handler_);
  }
  return *this;
}

namespace transport_test_access {

Subscription DeclarationHandleTestAccess::MakeSubscription(std::function<void()> on_reset) {
  return Subscription(std::move(on_reset));
}

Queryable DeclarationHandleTestAccess::MakeQueryable(std::function<void()> on_reset) {
  return Queryable(std::move(on_reset));
}

}  // namespace transport_test_access

std::unique_ptr<Transport> MakeZenohTransport() { return nullptr; }

}  // namespace sitos
