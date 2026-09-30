// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Typed query error reply payload codec (ADR-0036 §D6).

#include "src/transport/query_error_reply.hpp"

#include <gtest/gtest.h>

#include "sitos/transport.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using sitos::Status;
using sitos::transport_internal::DecodeQueryErrorReply;
using sitos::transport_internal::EncodeQueryErrorReply;
using sitos::transport_internal::IsQueryErrorStatus;

std::span<const std::byte> Bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

TEST(QueryErrorReplyTest, OnlyStateLostAndCatalogUnavailableAreTyped) {
  EXPECT_TRUE(IsQueryErrorStatus(Status::StateLost));
  EXPECT_TRUE(IsQueryErrorStatus(Status::CatalogUnavailable));
  for (Status status :
       {Status::Ok, Status::NotFound, Status::TypeMismatch, Status::Timeout, Status::Disconnected,
        Status::ReadOnly, Status::InvalidKey, Status::InvalidArgument, Status::Error,
        Status::OutcomeUnknown, static_cast<Status>(12), static_cast<Status>(255)}) {
    EXPECT_FALSE(IsQueryErrorStatus(status)) << static_cast<int>(status);
  }
}

TEST(QueryErrorReplyTest, EncodesExactCanonicalPayload) {
  EXPECT_EQ(EncodeQueryErrorReply(Status::StateLost), R"({"v":1,"status":10})");
  EXPECT_EQ(EncodeQueryErrorReply(Status::CatalogUnavailable), R"({"v":1,"status":11})");
}

TEST(QueryErrorReplyTest, DecodesOnlyCanonicalPayloads) {
  EXPECT_EQ(DecodeQueryErrorReply(Bytes(R"({"v":1,"status":10})")), Status::StateLost);
  EXPECT_EQ(DecodeQueryErrorReply(Bytes(R"({"v":1,"status":11})")), Status::CatalogUnavailable);

  for (std::string_view rejected : {
           "",
           "boom",
           R"({"v":1,"status":9})",
           R"({"v":1,"status":8})",
           R"({"v":1,"status":3})",
           R"({"v":1,"status":12})",
           R"({"v":2,"status":10})",
           R"({"v":1,"status":010})",
           R"({"v":1,"status":"10"})",
           R"({"status":10,"v":1})",
           R"({ "v":1,"status":10})",
           R"({"v":1,"status":10} )",
           R"({"v":1,"status":10,"extra":0})",
       }) {
    EXPECT_EQ(DecodeQueryErrorReply(Bytes(rejected)), std::nullopt) << rejected;
  }
}

TEST(QueryErrorReplyTest, RoundTripsEveryTypedStatus) {
  for (Status status : {Status::StateLost, Status::CatalogUnavailable}) {
    const std::string payload = EncodeQueryErrorReply(status);
    EXPECT_EQ(DecodeQueryErrorReply(Bytes(payload)), status);
  }
}

TEST(QueryErrorReplyTest, QueryWithoutNativeRequestCannotSendAnErrorReply) {
  // Runs in Zenoh-ON and Zenoh-OFF builds: validation precedes the native send,
  // and a query that has no native request reports a failure instead of replying.
  sitos::TransportQuery query;
  EXPECT_EQ(query.ReplyError(Status::NotFound).StatusCode(), Status::InvalidArgument);
  const auto sent = query.ReplyError(Status::StateLost);
  EXPECT_FALSE(sent.IsOk());
  EXPECT_NE(sent.StatusCode(), Status::InvalidArgument);
}

}  // namespace
