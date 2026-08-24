// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ADR-0028 low-level acknowledgement helper used by ParamStore (#17) and Fence
// (#158): generate one token, submit the data exactly once through the
// Transport adapter, then poll <prefix>/meta/ack/<uuid> within one total
// deadline. Internal to sitos; not an installed header.

#ifndef SITOS_ACK_CLIENT_HPP
#define SITOS_ACK_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "sitos/ack.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos {

/// Upper bound of one acknowledgement query window.
inline constexpr std::chrono::milliseconds kAckQueryWindowMax{1000};
/// Minimum pause between two acknowledgement queries.
inline constexpr std::chrono::milliseconds kAckQueryRetryDelay{100};

/// Submits one acknowledged Put/PutBatch and polls for its AckResultV1.
///
/// The total deadline starts immediately before the sole data Put. Each query
/// window is min(1000 ms, remaining); only one query is active at a time; at
/// least 100 ms separates two queries; there is no attempt-count limit. The
/// data write is never resubmitted.
///
/// Returns:
///  * Ok(result): one valid AckResultV1 was observed; `result.status` carries the
///    node's outcome (including OutcomeUnknown). Inspect it; Ok here does not mean
///    the write succeeded.
///  * Err(InvalidArgument): rejected before Put (non-positive deadline or invalid
///    prefix). Definitely NotSubmitted.
///  * Err(Error): a protocol error (wrong reply key or Encoding, or malformed
///    result) ended polling. MayHaveSubmitted.
///  * Err(Timeout): no valid result within the deadline; none, some, or all
///    effects may have occurred. The latest native Put/Get cause is retained for
///    diagnostics. MayHaveSubmitted. Never retry the data write on Timeout.
///
/// Synchronous; there is no cancellation API in v1 (DEC-14-ACK-CANCEL-001).
/// Polls an existing ADR-0028 token until an absolute total deadline.
/// Used by Fence after its sole marker submission.
[[nodiscard]] Result<AckResultV1> PollAcknowledgement(
    Transport& transport, std::string_view prefix, const AckToken& token,
    std::chrono::steady_clock::time_point deadline_at,
    std::optional<ErrorInfo> latest_transport_error = std::nullopt,
    std::function<std::optional<AckResultV1>()> cancelled = {},
    std::function<void(const AckResultV1&)> observed = {});

[[nodiscard]] Result<AckResultV1> SubmitAcknowledgedWrite(Transport& transport,
                                                          std::string_view prefix,
                                                          std::string_view full_key,
                                                          std::span<const std::byte> payload,
                                                          Encoding encoding,
                                                          std::chrono::milliseconds total_deadline);

}  // namespace sitos

#endif  // SITOS_ACK_CLIENT_HPP
