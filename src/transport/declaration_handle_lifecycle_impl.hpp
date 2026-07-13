// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared declaration-handle definitions. Include this internal header from the
// selected transport backend only after Subscription::Impl and Queryable::Impl
// are complete.

#pragma once

#include <utility>

#include "sitos/transport.hpp"
#include "declaration_handle_test_access.hpp"

namespace sitos {

Subscription::Subscription() = default;
Subscription::Subscription(std::function<void()> reset_handler)
    : reset_handler_(std::move(reset_handler)) {}
Subscription::~Subscription() { Reset(); }
Subscription::Subscription(Subscription&& other) noexcept {
  impl_.swap(other.impl_);
  reset_handler_.swap(other.reset_handler_);
}
Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_.swap(other.impl_);
    reset_handler_.swap(other.reset_handler_);
  }
  return *this;
}

Queryable::Queryable() = default;
Queryable::Queryable(std::function<void()> reset_handler)
    : reset_handler_(std::move(reset_handler)) {}
Queryable::~Queryable() { Reset(); }
Queryable::Queryable(Queryable&& other) noexcept {
  impl_.swap(other.impl_);
  reset_handler_.swap(other.reset_handler_);
}
Queryable& Queryable::operator=(Queryable&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_.swap(other.impl_);
    reset_handler_.swap(other.reset_handler_);
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
}  // namespace sitos
