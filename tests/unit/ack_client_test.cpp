// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the ADR-0028 low-level acknowledgement helper: one data
// submission, total-deadline query polling with min(1000 ms, remaining)
// windows and at-least-100 ms spacing, protocol-error precedence, and the
// conservative MayHaveSubmitted rule. Uses a scripted Transport; no Zenoh.

#include "ack_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/transport.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace {

using namespace std::chrono_literals;
using sitos::AckDurability;
using sitos::AckOperationKind;
using sitos::AckResultV1;
using sitos::AckToken;
using sitos::Encoding;
using sitos::kAckNoFailedIndex;
using sitos::kAckNoFailedSequence;
using sitos::Result;
using sitos::Status;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kPrefix = "sitos";
constexpr std::string_view kKey = "sitos/base/a";
const std::vector<std::byte> kPayload = {std::byte{0x01}, std::byte{0x02}};
const Encoding kV1{std::string(Encoding::kSitosV1)};
const Encoding kAckEncoding{std::string(Encoding::kSitosV1Ack)};

AckResultV1 PutOk() {
  return AckResultV1{AckOperationKind::Put, Status::Ok, AckDurability::Applied, 1,
                     kAckNoFailedIndex,     0,          kAckNoFailedSequence,   ""};
}

AckResultV1 PutUnknown() {
  return AckResultV1{AckOperationKind::Put, Status::OutcomeUnknown, AckDurability::Applied, 0, 0, 0,
                     kAckNoFailedSequence,  "engine refused"};
}

// Scripted Transport: records every Put and Get, answers each Get from a
// script, and can emulate a zero-reply window by sleeping for the timeout.
class ScriptedTransport final : public sitos::Transport {
 public:
  struct Step {
    enum class Kind { ZeroReplies, Reply, ReplyWrongKey, ReplyWrongEncoding, ReplyMalformed, Fail };
    Kind kind = Kind::ZeroReplies;
    std::optional<AckResultV1> result;
  };
  struct PutRecord {
    std::string key;
    std::vector<std::byte> payload;
    std::string encoding;
    std::optional<AckToken> token;
  };
  struct GetRecord {
    std::string keyexpr;
    std::chrono::milliseconds timeout;
    Clock::time_point at;
  };

  Result<void> Put(std::string_view key, std::span<const std::byte> payload, Encoding encoding,
                   sitos::PutOptions options) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      puts.push_back({std::string(key), std::vector<std::byte>(payload.begin(), payload.end()),
                      encoding.id, options.ack_token});
    }
    if (put_delay > 0ms) std::this_thread::sleep_for(put_delay);
    return put_result;
  }
  Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }
  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds timeout) override {
    Step step;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      gets.push_back({std::string(keyexpr), timeout, Clock::now()});
      if (!script.empty()) {
        step = script.front();
        script.erase(script.begin());
      }
    }
    switch (step.kind) {
      case Step::Kind::ZeroReplies:
        if (simulate_window) std::this_thread::sleep_for(timeout);
        return Result<void>::Ok();
      case Step::Kind::Reply: {
        const auto encoded = sitos::EncodeAckResult(*step.result);
        sink(keyexpr, encoded.Value(), kAckEncoding);
        return Result<void>::Ok();
      }
      case Step::Kind::ReplyWrongKey: {
        const auto encoded = sitos::EncodeAckResult(PutOk());
        sink("sitos/meta/ack/00000000-0000-4000-8000-000000000000", encoded.Value(), kAckEncoding);
        return Result<void>::Ok();
      }
      case Step::Kind::ReplyWrongEncoding: {
        const auto encoded = sitos::EncodeAckResult(PutOk());
        sink(keyexpr, encoded.Value(), kV1);
        return Result<void>::Ok();
      }
      case Step::Kind::ReplyMalformed: {
        const std::vector<std::byte> garbage(5, std::byte{0xFF});
        sink(keyexpr, garbage, kAckEncoding);
        return Result<void>::Ok();
      }
      case Step::Kind::Fail:
        return Result<void>::Err(Status::Disconnected, "link down",
                                 std::make_error_code(std::errc::not_connected));
    }
    return Result<void>::Ok();
  }
  Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)>) override {
    return Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }
  Result<sitos::Queryable> DeclareQueryable(std::string_view,
                                            std::function<void(sitos::TransportQuery&)>) override {
    return Result<sitos::Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

  std::vector<PutRecord> Puts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return puts;
  }
  std::vector<GetRecord> Gets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gets;
  }

  std::vector<Step> script;
  Result<void> put_result = Result<void>::Ok();
  std::chrono::milliseconds put_delay{0};
  bool simulate_window = false;

 private:
  mutable std::mutex mutex_;
  std::vector<PutRecord> puts;
  std::vector<GetRecord> gets;
};

ScriptedTransport::Step Reply(AckResultV1 result) {
  return {ScriptedTransport::Step::Kind::Reply, std::move(result)};
}
ScriptedTransport::Step Zero() { return {ScriptedTransport::Step::Kind::ZeroReplies, {}}; }
ScriptedTransport::Step Fail() { return {ScriptedTransport::Step::Kind::Fail, {}}; }

Result<AckResultV1> Submit(ScriptedTransport& transport, std::chrono::milliseconds deadline) {
  return sitos::SubmitAcknowledgedWrite(transport, kPrefix, kKey, kPayload, kV1, deadline);
}

// ---------------------------------------------------------------------------

TEST(AckClientTest, SubmitsDataExactlyOnceWithFreshTokenAndReturnsDecodedResult) {
  ScriptedTransport transport;
  transport.script = {Zero(), Zero(), Reply(PutOk())};
  const auto result = Submit(transport, 3000ms);
  ASSERT_TRUE(result.IsOk()) << result.Message();
  EXPECT_EQ(result.Value(), PutOk());

  const auto puts = transport.Puts();
  ASSERT_EQ(puts.size(), 1u) << "the data write is never resubmitted";
  EXPECT_EQ(puts[0].key, kKey);
  EXPECT_EQ(puts[0].payload, kPayload);
  EXPECT_EQ(puts[0].encoding, Encoding::kSitosV1);
  ASSERT_TRUE(puts[0].token.has_value());
  EXPECT_TRUE(sitos::IsValidAckToken(*puts[0].token));

  const auto gets = transport.Gets();
  ASSERT_EQ(gets.size(), 3u) << "only the acknowledgement query is retried";
  const std::string expected_key =
      std::string(kPrefix) + "/meta/ack/" + sitos::FormatAckToken(*puts[0].token);
  for (const auto& get : gets) {
    EXPECT_EQ(get.keyexpr, expected_key) << "the same token is preserved across attempts";
    EXPECT_EQ(get.timeout, 1000ms);
  }
}

TEST(AckClientTest, SaturatesVeryLargeDeadlineWithoutFalseTimeout) {
  ScriptedTransport transport;
  transport.script = {Reply(PutOk())};

  const auto result = Submit(transport, std::chrono::milliseconds::max());

  ASSERT_TRUE(result.IsOk()) << result.Message();
  EXPECT_EQ(result.Value(), PutOk());
  EXPECT_EQ(transport.Puts().size(), 1U);
  EXPECT_EQ(transport.Gets().size(), 1U);
}

TEST(AckClientTest, ResultStatusIsReturnedUnchanged) {
  ScriptedTransport transport;
  transport.script = {Reply(PutUnknown())};
  const auto result = Submit(transport, 1000ms);
  ASSERT_TRUE(result.IsOk());
  EXPECT_EQ(result.Value().status, Status::OutcomeUnknown);
  EXPECT_EQ(result.Value().message, "engine refused");
}

TEST(AckClientTest, PutAckTimesOutWhenNodeUnavailable) {
  ScriptedTransport transport;
  transport.simulate_window = true;  // every query window elapses with zero replies
  const auto start = Clock::now();
  const auto result = Submit(transport, 300ms);
  const auto elapsed = Clock::now() - start;

  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Timeout);
  EXPECT_GE(elapsed, 300ms) << "Timeout only when the total deadline expires";
  EXPECT_EQ(transport.Puts().size(), 1u) << "the data Put is still submitted exactly once";
  EXPECT_GE(transport.Gets().size(), 1u);
  for (const auto& get : transport.Gets()) EXPECT_GT(get.timeout.count(), 0);
}

TEST(AckClientTest, QueryWindowIsMinOfOneSecondAndRemainingDeadline) {
  ScriptedTransport transport;
  transport.simulate_window = true;
  const auto result = Submit(transport, 1500ms);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Timeout);

  const auto gets = transport.Gets();
  ASSERT_GE(gets.size(), 2u);
  EXPECT_EQ(gets[0].timeout, 1000ms);
  EXPECT_GT(gets[1].timeout.count(), 0);
  EXPECT_LE(gets[1].timeout, 500ms) << "second window is bounded by the remaining deadline";
  for (const auto& get : gets) EXPECT_LE(get.timeout, 1000ms);
}

TEST(AckClientTest, WaitsAtLeastOneHundredMillisecondsBetweenQueries) {
  ScriptedTransport transport;
  transport.script = {Zero(), Zero(), Zero(), Reply(PutOk())};
  ASSERT_TRUE(Submit(transport, 3000ms).IsOk());
  const auto gets = transport.Gets();
  ASSERT_EQ(gets.size(), 4u);
  for (std::size_t i = 1; i < gets.size(); ++i) {
    EXPECT_GE(gets[i].at - gets[i - 1].at, 100ms) << "between query " << i - 1 << " and " << i;
  }
}

TEST(AckClientTest, ProtocolErrorStopsPollingAndTakesPrecedence) {
  for (const auto kind : {ScriptedTransport::Step::Kind::ReplyWrongKey,
                          ScriptedTransport::Step::Kind::ReplyWrongEncoding,
                          ScriptedTransport::Step::Kind::ReplyMalformed}) {
    ScriptedTransport transport;
    transport.script = {ScriptedTransport::Step{kind, {}}, Reply(PutOk())};
    const auto result = Submit(transport, 3000ms);
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.StatusCode(), Status::Error);
    EXPECT_FALSE(result.Message().empty());
    EXPECT_EQ(transport.Gets().size(), 1u) << "polling stops after a protocol error";
    EXPECT_EQ(transport.Puts().size(), 1u);
  }
}

TEST(AckClientTest, PutFailureIsMayHaveSubmittedAndStillPollsUntilDeadline) {
  ScriptedTransport transport;
  transport.put_result = Result<void>::Err(Status::Disconnected, "session closed",
                                           std::make_error_code(std::errc::not_connected));
  transport.script = {Zero(), Reply(PutOk())};
  const auto result = Submit(transport, 3000ms);
  ASSERT_TRUE(result.IsOk()) << "a result observed after a non-OK Put still wins";
  EXPECT_EQ(transport.Puts().size(), 1u);
  EXPECT_EQ(transport.Gets().size(), 2u);

  ScriptedTransport silent;
  silent.put_result = transport.put_result;
  silent.simulate_window = true;
  const auto timeout = Submit(silent, 250ms);
  ASSERT_FALSE(timeout.IsOk());
  EXPECT_EQ(timeout.StatusCode(), Status::Timeout);
  EXPECT_EQ(timeout.Error(), std::make_error_code(std::errc::not_connected))
      << "the latest native cause is retained for diagnostics";
  EXPECT_EQ(silent.Puts().size(), 1u) << "never resubmitted";
}

TEST(AckClientTest, GetFailureIsRetriedUntilDeadline) {
  ScriptedTransport transport;
  transport.script = {Fail(), Fail(), Reply(PutOk())};
  ASSERT_TRUE(Submit(transport, 3000ms).IsOk());
  EXPECT_EQ(transport.Gets().size(), 3u);

  ScriptedTransport always_failing;
  always_failing.script = std::vector<ScriptedTransport::Step>(50, Fail());
  const auto result = Submit(always_failing, 250ms);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Timeout);
  EXPECT_EQ(result.Error(), std::make_error_code(std::errc::not_connected));
  EXPECT_EQ(always_failing.Puts().size(), 1u);
}

TEST(AckClientTest, SubmissionConsumingTheDeadlineReturnsTimeoutWithoutQuery) {
  ScriptedTransport transport;
  transport.put_delay = 150ms;
  const auto result = Submit(transport, 100ms);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Timeout);
  EXPECT_EQ(transport.Puts().size(), 1u);
  EXPECT_TRUE(transport.Gets().empty()) << "no non-positive query window is opened";
}

TEST(AckClientTest, CanonicalEmptyBatchShortCircuitsWithoutTokenSubmissionOrQuery) {
  // ADR-0028: an empty PutBatch returns immediate Ok with no data submission, token,
  // or query.
  ScriptedTransport transport;
  const std::vector<std::byte> empty_batch = {std::byte{0}, std::byte{0}, std::byte{0},
                                              std::byte{0}};
  const Encoding batch_encoding{std::string(Encoding::kSitosV1Batch)};
  const auto result = sitos::SubmitAcknowledgedWrite(transport, kPrefix, "sitos/base/:batch",
                                                     empty_batch, batch_encoding, 3000ms);
  ASSERT_TRUE(result.IsOk()) << result.Message();
  EXPECT_EQ(result.Value().operation_kind, AckOperationKind::Batch);
  EXPECT_EQ(result.Value().status, Status::Ok);
  EXPECT_EQ(result.Value().applied_count, 0u);
  EXPECT_EQ(result.Value().failed_index, kAckNoFailedIndex);
  EXPECT_TRUE(transport.Puts().empty()) << "no data submission";
  EXPECT_TRUE(transport.Gets().empty()) << "no acknowledgement query";

  // A malformed batch payload is not a canonical empty batch and is still submitted.
  ScriptedTransport malformed;
  malformed.script = {Reply(PutOk())};
  const std::vector<std::byte> trailing = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                           std::byte{0xFF}};
  static_cast<void>(sitos::SubmitAcknowledgedWrite(malformed, kPrefix, "sitos/base/:batch",
                                                   trailing, batch_encoding, 1000ms));
  EXPECT_EQ(malformed.Puts().size(), 1u);

  // A non-empty batch is submitted normally.
  ScriptedTransport nonempty;
  nonempty.script = {Reply(PutOk())};
  const std::vector<std::byte> one_entry = {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                                            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                                            std::byte{'k'}, std::byte{0}, std::byte{1},
                                            std::byte{0}, std::byte{0}, std::byte{0},
                                            std::byte{1}};
  static_cast<void>(sitos::SubmitAcknowledgedWrite(nonempty, kPrefix, "sitos/base/:batch",
                                                   one_entry, batch_encoding, 1000ms));
  EXPECT_EQ(nonempty.Puts().size(), 1u);
}

TEST(AckClientTest, InvalidInputsAreRejectedBeforeSubmission) {
  ScriptedTransport transport;
  EXPECT_EQ(Submit(transport, 0ms).StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(Submit(transport, -5ms).StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(sitos::SubmitAcknowledgedWrite(transport, "bad//prefix", kKey, kPayload, kV1, 1000ms)
                .StatusCode(),
            Status::InvalidArgument);
  EXPECT_TRUE(transport.Puts().empty()) << "definite NotSubmitted";
  EXPECT_TRUE(transport.Gets().empty());
}

}  // namespace
