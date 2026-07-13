// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Internal test access for transport-independent declaration handle lifetimes.

#ifndef SITOS_TRANSPORT_DECLARATION_HANDLE_TEST_ACCESS_HPP
#define SITOS_TRANSPORT_DECLARATION_HANDLE_TEST_ACCESS_HPP

#include <functional>

#include "sitos/transport.hpp"

namespace sitos::transport_test_access {

class DeclarationHandleTestAccess {
 public:
  static Subscription MakeSubscription(std::function<void()> on_reset);
  static Queryable MakeQueryable(std::function<void()> on_reset);
};

}  // namespace sitos::transport_test_access

#endif  // SITOS_TRANSPORT_DECLARATION_HANDLE_TEST_ACCESS_HPP
