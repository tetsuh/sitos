// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Internal test access for validating ZenohTransport behavior.

#ifndef SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP
#define SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "sitos/transport.hpp"

namespace sitos::transport_test_access {

/// Returns the actual zenoh-c string produced by the production Encoding builder.
std::optional<std::string> BuildWireEncoding(const Encoding& encoding);

/// Applies the production receive-side normalization to a wire Encoding string.
Encoding NormalizeWireEncoding(std::string wire_encoding);

/// Returns the production diagnostic for a native zenoh-c result value.
std::error_code MakeNativeError(std::int8_t code);

/// Verifies allocation failure occurs before a constructor argument transfers ownership.
bool AllocationFailurePrecedesOwnershipTransfer();

/// Returns a production transport with no opened Zenoh session.
std::unique_ptr<Transport> MakeDisconnectedTransport();

/// Internal access for Subscription ownership regression tests.
class SubscriptionTestAccess {
 public:
  static bool IsAvailable();
  static void Shutdown();
  static Subscription Make(std::string_view keyexpr, std::function<void()> callback);
  static bool Publish(std::string_view keyexpr);
};

}  // namespace sitos::transport_test_access

#endif  // SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP
