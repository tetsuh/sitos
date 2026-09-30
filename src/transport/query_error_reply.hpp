// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Typed query error reply payload (ADR-0036 §D6). A StorageNode refuses a
// query with a Zenoh error reply whose payload is exactly {"v":1,"status":<n>};
// only StateLost and CatalogUnavailable are typed on this surface.

#ifndef SITOS_TRANSPORT_QUERY_ERROR_REPLY_HPP
#define SITOS_TRANSPORT_QUERY_ERROR_REPLY_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "sitos/status.hpp"

namespace sitos::transport_internal {

/// Returns true only for the Status values a typed error reply may carry.
bool IsQueryErrorStatus(Status status) noexcept;

/// Returns the canonical payload. Requires IsQueryErrorStatus(status).
std::string EncodeQueryErrorReply(Status status);

/// Returns the typed Status for an exact canonical payload, otherwise nullopt.
std::optional<Status> DecodeQueryErrorReply(std::span<const std::byte> payload) noexcept;

}  // namespace sitos::transport_internal

#endif  // SITOS_TRANSPORT_QUERY_ERROR_REPLY_HPP
