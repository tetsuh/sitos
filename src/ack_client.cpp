// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "ack_client.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/key.hpp"

namespace sitos {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kDeadlineNotPositive = "acknowledgement deadline must be positive";
constexpr std::string_view kInvalidPrefix = "acknowledgement prefix is invalid";
constexpr std::string_view kNoAckWithinDeadline = "no acknowledgement within the total deadline";
constexpr std::string_view kProtocolWrongKey = "ack protocol error: reply key differs from query";
constexpr std::string_view kProtocolWrongEncoding =
    "ack protocol error: reply Encoding is not sitos.v1.ack";
constexpr std::string_view kProtocolMalformed = "ack protocol error: malformed AckResultV1";

std::string TimeoutMessage(const std::string& latest_message) {
  std::string message(kNoAckWithinDeadline);
  if (!latest_message.empty()) message += "; last transport error: " + latest_message;
  return message;
}

}  // namespace

Result<AckResultV1> SubmitAcknowledgedWrite(Transport& transport, std::string_view prefix,
                                            std::string_view full_key,
                                            std::span<const std::byte> payload, Encoding encoding,
                                            std::chrono::milliseconds total_deadline) {
  using R = Result<AckResultV1>;
  // Definite NotSubmitted: validation before the Transport is invoked.
  if (total_deadline.count() <= 0) {
    return R::Err(Status::InvalidArgument, std::string(kDeadlineNotPositive));
  }
  const AckToken token = GenerateAckToken();
  const auto query_key = BuildMetaAckKey(prefix, FormatAckToken(token));
  if (!query_key) return R::Err(Status::InvalidArgument, std::string(kInvalidPrefix));

  // The total deadline starts immediately before the sole data submission.
  const Clock::time_point deadline_at = Clock::now() + total_deadline;
  PutOptions options;
  options.ack_token = token;
  std::error_code latest_cause;
  std::string latest_message;
  if (auto put = transport.Put(full_key, payload, std::move(encoding), options); !put.IsOk()) {
    // Conservatively MayHaveSubmitted: keep polling; never resubmit.
    latest_cause = put.Error();
    latest_message = std::string(put.Message());
  }

  for (;;) {
    // Round up so that a sub-millisecond remainder still opens a 1 ms window: Timeout is
    // reported only after the total deadline has actually expired.
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline_at - Clock::now());
    if (remaining.count() <= 0) {
      return R::Err(Status::Timeout, TimeoutMessage(latest_message), latest_cause);
    }
    const auto window = std::min(kAckQueryWindowMax, remaining);

    std::optional<AckResultV1> observed;
    std::optional<std::string_view> protocol_error;
    const auto get = transport.Get(
        *query_key,
        [&](std::string_view key, std::span<const std::byte> reply, Encoding reply_encoding) {
          if (key != *query_key) {
            protocol_error = kProtocolWrongKey;
          } else if (reply_encoding.id != Encoding::kSitosV1Ack) {
            protocol_error = kProtocolWrongEncoding;
          } else if (auto decoded = DecodeAckResult(reply); !decoded.IsOk()) {
            protocol_error = kProtocolMalformed;
          } else {
            observed = std::move(decoded).Value();
          }
          return false;  // at most one consolidated reply is meaningful
        },
        window);

    // After Get quiesces: a protocol error takes precedence; otherwise one valid
    // decoded result; otherwise the failure or zero reply is retried.
    if (protocol_error) return R::Err(Status::Error, std::string(*protocol_error));
    if (observed) return R::Ok(std::move(*observed));
    if (!get.IsOk()) {
      latest_cause = get.Error();
      latest_message = std::string(get.Message());
    }

    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline_at - Clock::now());
    if (left.count() <= 0) {
      return R::Err(Status::Timeout, TimeoutMessage(latest_message), latest_cause);
    }
    std::this_thread::sleep_for(std::min(kAckQueryRetryDelay, left));
  }
}

}  // namespace sitos
