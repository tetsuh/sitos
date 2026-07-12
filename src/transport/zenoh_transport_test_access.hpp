// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Internal test access for validating ZenohTransport wire encodings.

#ifndef SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP
#define SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP

#include <optional>
#include <string>

#include "sitos/transport.hpp"

namespace sitos::transport_test_access {

/// Returns the actual zenoh-c string produced by the production Encoding builder.
std::optional<std::string> BuildWireEncoding(const Encoding& encoding);

/// Applies the production receive-side normalization to a wire Encoding string.
Encoding NormalizeWireEncoding(std::string wire_encoding);

}  // namespace sitos::transport_test_access

#endif  // SITOS_TRANSPORT_ZENOH_TRANSPORT_TEST_ACCESS_HPP
