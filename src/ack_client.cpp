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

#include "sitos/batch.hpp"
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

namespace ack_client_internal {

Clock::time_point SaturatingDeadlineAt(Clock::time_point now, std::chrono::milliseconds requested) {
  const auto remaining = Clock::time_point::max() - now;
  const auto remaining_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (requested > remaining_milliseconds) return Clock::time_point::max();
  return now + std::chrono::duration_cast<Clock::duration>(requested);
}

}  // namespace ack_client_internal

namespace {

Clock::time_point SaturatingDeadline(std::chrono::milliseconds requested) {
  return ack_client_internal::SaturatingDeadlineAt(Clock::now(), requested);
}

}  // namespace

Result<AckResultV1> PollAcknowledgement(Transport& transport, std::string_view prefix,
                                        const AckToken& token, Clock::time_point deadline_at,
                                        std::optional<ErrorInfo> latest_transport_error,
                                        std::function<std::optional<AckResultV1>()> cancelled,
                                        std::function<void(const AckResultV1&)> observed_callback) {
  using R = Result<AckResultV1>;
  const auto query_key = BuildMetaAckKey(prefix, FormatAckToken(token));
  if (!query_key) return R::Err(Status::InvalidArgument, std::string(kInvalidPrefix));
  std::error_code latest_cause;
  std::string latest_message;
  if (latest_transport_error.has_value()) {
    latest_cause = latest_transport_error->cause;
    latest_message = latest_transport_error->message;
  }

  for (;;) {
    if (cancelled) {
      if (auto result = cancelled(); result.has_value()) return R::Ok(std::move(*result));
    }
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
            if (observed_callback) observed_callback(*observed);
          }
          return false;
        },
        window);

    if (cancelled) {
      if (auto result = cancelled(); result.has_value()) return R::Ok(std::move(*result));
    }
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

Result<AckResultV1> SubmitAcknowledgedWrite(Transport& transport, std::string_view prefix,
                                            std::string_view full_key,
                                            std::span<const std::byte> payload, Encoding encoding,
                                            std::chrono::milliseconds total_deadline) {
  using R = Result<AckResultV1>;
  if (total_deadline.count() <= 0) {
    return R::Err(Status::InvalidArgument, std::string(kDeadlineNotPositive));
  }
  if (encoding.id == Encoding::kSitosV1Batch) {
    if (auto decoded = DecodeBatch(payload); decoded.has_value() && decoded->empty()) {
      return R::Ok(AckResultV1{AckOperationKind::Batch, Status::Ok, AckDurability::Applied, 0,
                               kAckNoFailedIndex, 0, kAckNoFailedSequence, ""});
    }
  }
  const AckToken token = GenerateAckToken();
  if (!BuildMetaAckKey(prefix, FormatAckToken(token))) {
    return R::Err(Status::InvalidArgument, std::string(kInvalidPrefix));
  }

  const Clock::time_point deadline_at = SaturatingDeadline(total_deadline);
  PutOptions options;
  options.ack_token = token;
  std::optional<ErrorInfo> latest_error;
  if (auto put = transport.Put(full_key, payload, std::move(encoding), options); !put.IsOk()) {
    latest_error = ErrorInfo{put.StatusCode(), std::string(put.Message()), put.Error()};
  }
  return PollAcknowledgement(transport, prefix, token, deadline_at, std::move(latest_error));
}

}  // namespace sitos
