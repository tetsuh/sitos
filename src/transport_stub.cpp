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
Subscription::~Subscription() = default;
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&&) noexcept = default;

Queryable::Queryable() = default;
Queryable::~Queryable() { Reset(); }
void Queryable::Reset() noexcept { impl_.reset(); }
Queryable::Queryable(Queryable&&) noexcept = default;
Queryable& Queryable::operator=(Queryable&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}

}  // namespace sitos
