// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the ADR-0028 acknowledgement substrate: UUIDv4 tokens, the
// 17-byte AckAttachmentV1 attachment, the three-state attachment observation,
// and the AckResultV1 wire codec (golden fixtures in tests/fixtures/ack_v1/).

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/status.hpp"
#include "sitos/transport.hpp"

namespace {

using sitos::AckAttachmentAbsent;
using sitos::AckAttachmentMalformed;
using sitos::AckAttachmentObservation;
using sitos::AckDurability;
using sitos::AckOperationKind;
using sitos::AckResultV1;
using sitos::AckToken;
using sitos::kAckNoFailedIndex;
using sitos::kAckNoFailedSequence;
using sitos::Status;

std::vector<std::byte> LoadFixture(const std::string& name) {
  const std::string path = std::string(SITOS_ACK_FIXTURE_DIR) + "/" + name + ".hex";
  std::ifstream f(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string hex;
  for (char c : content) {
    if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
  }
  EXPECT_FALSE(hex.empty()) << "fixture " << name << " is empty / missing";
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16))));
  }
  return out;
}

constexpr std::string_view kTokenText = "550e8400-e29b-41d4-a716-446655440000";

AckToken FixtureToken() {
  return AckToken{{std::byte{0x55}, std::byte{0x0e}, std::byte{0x84}, std::byte{0x00},
                   std::byte{0xe2}, std::byte{0x9b}, std::byte{0x41}, std::byte{0xd4},
                   std::byte{0xa7}, std::byte{0x16}, std::byte{0x44}, std::byte{0x66},
                   std::byte{0x55}, std::byte{0x44}, std::byte{0x00}, std::byte{0x00}}};
}

std::span<const std::byte> Span(const std::vector<std::byte>& v) { return v; }

// Independent raw encoder: writes the ADR-0028 layout without any validation so
// that invariant-violating byte strings can be produced for decode tests.
void PutLe(std::vector<std::byte>& out, std::uint64_t value, int size) {
  for (int i = 0; i < size; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

std::vector<std::byte> RawEncode(const AckResultV1& r, std::uint8_t version = 1) {
  std::vector<std::byte> out;
  out.push_back(std::byte{version});
  out.push_back(static_cast<std::byte>(r.operation_kind));
  out.push_back(static_cast<std::byte>(r.status));
  out.push_back(static_cast<std::byte>(r.durability));
  PutLe(out, r.applied_count, 4);
  PutLe(out, r.failed_index, 4);
  PutLe(out, r.through_sequence, 8);
  PutLe(out, r.failed_sequence, 8);
  PutLe(out, r.message.size(), 4);
  for (unsigned char c : r.message) out.push_back(std::byte{c});
  return out;
}

AckResultV1 PutOk() {
  return AckResultV1{AckOperationKind::Put, Status::Ok, AckDurability::Applied, 1,
                     kAckNoFailedIndex,     0,          kAckNoFailedSequence,   ""};
}

AckResultV1 PutUnknown(std::string message = "") {
  return AckResultV1{AckOperationKind::Put, Status::OutcomeUnknown, AckDurability::Applied, 0, 0, 0,
                     kAckNoFailedSequence,  std::move(message)};
}

AckResultV1 BatchOk(std::uint32_t count) {
  return AckResultV1{AckOperationKind::Batch, Status::Ok, AckDurability::Applied, count,
                     kAckNoFailedIndex,       0,          kAckNoFailedSequence,   ""};
}

AckResultV1 BatchFailed(Status status, std::uint32_t applied, std::uint32_t failed_index) {
  return AckResultV1{
      AckOperationKind::Batch, status, AckDurability::Applied, applied, failed_index, 0,
      kAckNoFailedSequence,    ""};
}

AckResultV1 Fence(Status status, AckDurability durability, std::uint64_t through,
                  std::uint64_t failed_sequence, std::string message = "") {
  return AckResultV1{AckOperationKind::Fence, status,  durability,      0,
                     kAckNoFailedIndex,       through, failed_sequence, std::move(message)};
}

// Asserts that both the encoder and the decoder reject the same invariant violation.
void ExpectInvalid(const AckResultV1& r, std::string_view why) {
  const auto encoded = sitos::EncodeAckResult(r);
  EXPECT_FALSE(encoded.IsOk()) << "encode accepted: " << why;
  const auto decoded = sitos::DecodeAckResult(RawEncode(r));
  EXPECT_FALSE(decoded.IsOk()) << "decode accepted: " << why;
  EXPECT_EQ(sitos::ValidateAckResult(r).StatusCode(), Status::InvalidArgument) << why;
}

void ExpectValid(const AckResultV1& r, std::string_view why) {
  const auto encoded = sitos::EncodeAckResult(r);
  ASSERT_TRUE(encoded.IsOk()) << why << ": " << encoded.Message();
  EXPECT_EQ(encoded.Value(), RawEncode(r)) << why;
  const auto decoded = sitos::DecodeAckResult(encoded.Value());
  ASSERT_TRUE(decoded.IsOk()) << why << ": " << decoded.Message();
  EXPECT_EQ(decoded.Value(), r) << why;
}

// ---------------------------------------------------------------------------
// Status append
// ---------------------------------------------------------------------------

TEST(AckStatusTest, OutcomeUnknownIsAppendedWithoutRenumbering) {
  EXPECT_EQ(static_cast<int>(Status::Error), 8);
  EXPECT_EQ(static_cast<int>(Status::OutcomeUnknown), 9);
  const auto code = sitos::MakeErrorCode(Status::OutcomeUnknown);
  EXPECT_EQ(code.value(), 9);
  EXPECT_STREQ(code.category().name(), "sitos.status");
  EXPECT_EQ(code.message(), "outcome unknown");
}

// ---------------------------------------------------------------------------
// UUIDv4 tokens
// ---------------------------------------------------------------------------

TEST(AckTokenTest, FormatProducesCanonicalLowercaseText) {
  EXPECT_EQ(sitos::FormatAckToken(FixtureToken()), kTokenText);
  AckToken upper_nibbles{};
  upper_nibbles.bytes.fill(std::byte{0xAB});
  upper_nibbles.bytes[6] = std::byte{0x4B};
  upper_nibbles.bytes[8] = std::byte{0xBB};
  EXPECT_EQ(sitos::FormatAckToken(upper_nibbles), "abababab-abab-4bab-bbab-abababababab");
}

TEST(AckTokenTest, ParseAcceptsOnlyCanonicalV4Text) {
  const auto parsed = sitos::ParseAckToken(kTokenText);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, FixtureToken());

  EXPECT_FALSE(sitos::ParseAckToken("").has_value());
  EXPECT_FALSE(sitos::ParseAckToken("550E8400-E29B-41D4-A716-446655440000").has_value())
      << "uppercase is not canonical";
  EXPECT_FALSE(sitos::ParseAckToken("{550e8400-e29b-41d4-a716-446655440000}").has_value());
  EXPECT_FALSE(sitos::ParseAckToken("550e8400e29b41d4a716446655440000").has_value())
      << "hyphens are required";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-41d4-a716-44665544000").has_value())
      << "too short";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-41d4-a716-4466554400000").has_value())
      << "too long";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400e-29b-41d4-a716-446655440000").has_value())
      << "misplaced hyphen";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-41d4-a716-44665544000g").has_value())
      << "non-hex digit";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-11d4-a716-446655440000").has_value())
      << "version 1 is not v4";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-41d4-2716-446655440000").has_value())
      << "variant 0xxx is not RFC 4122";
  EXPECT_FALSE(sitos::ParseAckToken("550e8400-e29b-41d4-c716-446655440000").has_value())
      << "variant 110x is not RFC 4122";
}

TEST(AckTokenTest, IsValidRequiresVersion4AndRfc4122Variant) {
  EXPECT_TRUE(sitos::IsValidAckToken(FixtureToken()));
  AckToken token = FixtureToken();
  token.bytes[6] = std::byte{0x1d};
  EXPECT_FALSE(sitos::IsValidAckToken(token));
  token = FixtureToken();
  token.bytes[8] = std::byte{0x27};
  EXPECT_FALSE(sitos::IsValidAckToken(token));
  token = FixtureToken();
  token.bytes[8] = std::byte{0xe7};
  EXPECT_FALSE(sitos::IsValidAckToken(token));
  AckToken zero{};
  EXPECT_FALSE(sitos::IsValidAckToken(zero));
}

TEST(AckTokenTest, GenerateProducesDistinctCanonicalV4Tokens) {
  std::set<std::string> seen;
  for (int i = 0; i < 256; ++i) {
    const AckToken token = sitos::GenerateAckToken();
    ASSERT_TRUE(sitos::IsValidAckToken(token));
    const std::string text = sitos::FormatAckToken(token);
    ASSERT_EQ(text.size(), 36u);
    const auto parsed = sitos::ParseAckToken(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, token);
    seen.insert(text);
  }
  EXPECT_EQ(seen.size(), 256u);
}

// ---------------------------------------------------------------------------
// AckAttachmentV1
// ---------------------------------------------------------------------------

TEST(AckAttachmentTest, GoldenFixture) {
  const auto fixture = LoadFixture("attachment_put_token");
  ASSERT_EQ(fixture.size(), sitos::kAckAttachmentV1Size);

  const auto encoded = sitos::EncodeAckAttachment(FixtureToken());
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), fixture.begin(), fixture.end()));

  const AckAttachmentObservation observed = sitos::ObserveAckAttachment(Span(fixture));
  ASSERT_TRUE(std::holds_alternative<AckToken>(observed));
  EXPECT_EQ(std::get<AckToken>(observed), FixtureToken());
}

TEST(AckAttachmentTest, ObserveDistinguishesAbsentValidAndMalformed) {
  EXPECT_EQ(sitos::ObserveAckAttachment(std::nullopt),
            AckAttachmentObservation{AckAttachmentAbsent{}});

  const std::vector<std::byte> empty;
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(empty)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "a present but empty attachment is malformed, not absent";

  const auto valid = LoadFixture("attachment_put_token");
  std::vector<std::byte> bytes(valid.begin(), valid.end() - 1);
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "16 bytes";
  bytes = valid;
  bytes.push_back(std::byte{0});
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "18 bytes";
  bytes = valid;
  bytes[0] = std::byte{0};
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "schema_version 0";
  bytes = valid;
  bytes[0] = std::byte{2};
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "schema_version 2";
  bytes = valid;
  bytes[7] = std::byte{0x1d};
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "non-v4 UUID";
  bytes = valid;
  bytes[9] = std::byte{0x27};
  EXPECT_EQ(sitos::ObserveAckAttachment(Span(bytes)),
            AckAttachmentObservation{AckAttachmentMalformed{}})
      << "non-RFC-4122 variant";
}

// ---------------------------------------------------------------------------
// AckResultV1
// ---------------------------------------------------------------------------

TEST(AckResultTest, GoldenFixtures) {
  struct Case {
    const char* name;
    AckResultV1 expected;
  };
  const Case cases[] = {
      {"result_put_ok", PutOk()},
      {"result_put_outcome_unknown", PutUnknown("engine: 失敗")},
      {"result_batch_ok", BatchOk(3)},
      {"result_batch_envelope_invalid", BatchFailed(Status::InvalidArgument, 0, kAckNoFailedIndex)},
      {"result_batch_entry_invalid", BatchFailed(Status::InvalidArgument, 0, 1)},
      {"result_batch_prefix_unknown", BatchFailed(Status::OutcomeUnknown, 2, 2)},
      {"result_fence_synced_ok",
       Fence(Status::Ok, AckDurability::Synced, 42, kAckNoFailedSequence)},
      {"result_fence_failed_sequence",
       Fence(Status::Error, AckDurability::Applied, 5, 3, "lane 3 failed")},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    const auto fixture = LoadFixture(c.name);
    const auto decoded = sitos::DecodeAckResult(Span(fixture));
    ASSERT_TRUE(decoded.IsOk()) << decoded.Message();
    EXPECT_EQ(decoded.Value(), c.expected);
    const auto encoded = sitos::EncodeAckResult(c.expected);
    ASSERT_TRUE(encoded.IsOk()) << encoded.Message();
    EXPECT_EQ(encoded.Value(), fixture);
  }
}

TEST(AckResultTest, RejectsMalformedEnvelope) {
  const auto ok = RawEncode(PutOk());
  ASSERT_EQ(ok.size(), sitos::kAckResultV1HeaderSize);

  std::vector<std::byte> bytes(ok.begin(), ok.end() - 1);
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "truncated header";
  EXPECT_FALSE(sitos::DecodeAckResult(std::span<const std::byte>{}).IsOk()) << "empty";

  bytes = ok;
  bytes.push_back(std::byte{0});
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "trailing byte";

  bytes = RawEncode(PutUnknown("ab"));
  bytes.pop_back();
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "message shorter than declared";

  bytes = RawEncode(PutUnknown("ab"));
  bytes.push_back(std::byte{'c'});
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "message longer than declared";

  bytes = RawEncode(PutOk(), /*version=*/2);
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "unknown schema_version";
  bytes = RawEncode(PutOk(), /*version=*/0);
  EXPECT_FALSE(sitos::DecodeAckResult(Span(bytes)).IsOk()) << "schema_version 0";

  ExpectValid(PutUnknown(std::string(sitos::kAckResultMaxMessageLength, 'x')),
              "1024-byte message is the maximum");
  ExpectInvalid(PutUnknown(std::string(sitos::kAckResultMaxMessageLength + 1, 'x')),
                "1025-byte message");
}

TEST(AckResultTest, RejectsUnknownEnumsAndWireForbiddenStatus) {
  AckResultV1 r = PutOk();
  r.operation_kind = static_cast<AckOperationKind>(0);
  ExpectInvalid(r, "operation_kind 0");
  r.operation_kind = static_cast<AckOperationKind>(4);
  ExpectInvalid(r, "operation_kind 4");

  r = PutOk();
  r.durability = static_cast<AckDurability>(0);
  ExpectInvalid(r, "durability 0");
  r.durability = static_cast<AckDurability>(3);
  ExpectInvalid(r, "durability 3");
  r.durability = AckDurability::Synced;
  ExpectInvalid(r, "put durability is always applied");
  r = BatchOk(1);
  r.durability = AckDurability::Synced;
  ExpectInvalid(r, "batch durability is always applied");

  r = PutUnknown();
  r.status = Status::Timeout;
  ExpectInvalid(r, "Timeout is client-only and rejected on the wire");
  r.status = static_cast<Status>(10);
  ExpectInvalid(r, "future Status value");
  r.status = static_cast<Status>(255);
  ExpectInvalid(r, "Status 255");

  // Every allowlisted non-OK status is accepted for a put failure.
  for (Status status :
       {Status::NotFound, Status::TypeMismatch, Status::Disconnected, Status::ReadOnly,
        Status::InvalidKey, Status::InvalidArgument, Status::Error, Status::OutcomeUnknown}) {
    r = PutUnknown();
    r.status = status;
    ExpectValid(r, "allowlisted status");
  }
}

TEST(AckResultTest, RejectsInvalidUtf8Message) {
  const std::vector<std::string> invalid = {
      std::string("\xFF", 1),                  // never valid
      std::string("\xC0\x80", 2),              // overlong NUL
      std::string("\xE0\x80\xAF", 3),          // overlong 3-byte
      std::string("\xED\xA0\x80", 3),          // UTF-16 surrogate
      std::string("\xF4\x90\x80\x80", 4),      // above U+10FFFF
      std::string("\xE5\xA4", 2),              // truncated sequence
      std::string("a\x80", 2),                 // stray continuation
      std::string("\xF8\x88\x80\x80\x80", 5),  // 5-byte form
  };
  for (const auto& message : invalid) {
    ExpectInvalid(PutUnknown(message), "invalid UTF-8");
  }
  ExpectValid(PutUnknown(std::string("\xF0\x9F\x98\x80 ok", 7)), "4-byte UTF-8 is valid");
  ExpectValid(PutUnknown(std::string(1, '\0')), "NUL is valid UTF-8");
}

TEST(AckResultTest, EnforcesPutInvariants) {
  ExpectValid(PutOk(), "put success");
  ExpectValid(PutUnknown(), "put failure");

  AckResultV1 r = PutOk();
  r.applied_count = 2;
  ExpectInvalid(r, "put success applied_count must be 1");
  r = PutOk();
  r.applied_count = 0;
  ExpectInvalid(r, "put success applied_count must be 1");
  r = PutOk();
  r.failed_index = 0;
  ExpectInvalid(r, "put success has no failed_index");

  r = PutUnknown();
  r.applied_count = 1;
  ExpectInvalid(r, "put failure applied_count must be 0");
  r = PutUnknown();
  r.failed_index = kAckNoFailedIndex;
  ExpectInvalid(r, "put failure failed_index must be 0");
  r = PutUnknown();
  r.failed_index = 1;
  ExpectInvalid(r, "put failure failed_index must be 0");

  r = PutOk();
  r.through_sequence = 1;
  ExpectInvalid(r, "put through_sequence is not applicable");
  r = PutOk();
  r.failed_sequence = 0;
  ExpectInvalid(r, "put failed_sequence must be the none sentinel");
}

TEST(AckResultTest, EnforcesBatchInvariants) {
  ExpectValid(BatchOk(0), "batch success with zero entries");
  ExpectValid(BatchOk(7), "batch success");
  ExpectValid(BatchFailed(Status::InvalidArgument, 0, kAckNoFailedIndex), "envelope failure");
  ExpectValid(BatchFailed(Status::InvalidArgument, 0, 4), "entry validation failure");
  ExpectValid(BatchFailed(Status::OutcomeUnknown, 4, 4), "application failure after prefix");
  ExpectValid(BatchFailed(Status::Error, 0, 0), "first entry failed");

  AckResultV1 r = BatchOk(2);
  r.failed_index = 1;
  ExpectInvalid(r, "batch success has no failed_index");
  ExpectInvalid(BatchFailed(Status::Error, 1, kAckNoFailedIndex),
                "batch failure with applied prefix must name the failed entry");
  ExpectInvalid(BatchFailed(Status::Error, 1, 3),
                "applied_count must be 0 or equal to failed_index");
  ExpectInvalid(BatchFailed(Status::Error, 3, 2), "applied_count beyond failed_index");

  r = BatchOk(1);
  r.through_sequence = 9;
  ExpectInvalid(r, "batch through_sequence is not applicable");
  r = BatchOk(1);
  r.failed_sequence = 1;
  ExpectInvalid(r, "batch failed_sequence must be the none sentinel");
}

TEST(AckResultTest, EnforcesFenceInvariants) {
  ExpectValid(Fence(Status::Ok, AckDurability::Applied, 0, kAckNoFailedSequence),
              "fence over an empty prefix");
  ExpectValid(Fence(Status::Ok, AckDurability::Synced, 10, kAckNoFailedSequence), "synced fence");
  ExpectValid(Fence(Status::Error, AckDurability::Applied, 10, 10),
              "failed_sequence equal to through_sequence");
  ExpectValid(Fence(Status::Error, AckDurability::Applied, 10, 1), "failed_sequence within prefix");

  AckResultV1 r = Fence(Status::Ok, AckDurability::Applied, 3, kAckNoFailedSequence);
  r.applied_count = 1;
  ExpectInvalid(r, "fence applied_count must be 0");
  r = Fence(Status::Ok, AckDurability::Applied, 3, kAckNoFailedSequence);
  r.failed_index = 0;
  ExpectInvalid(r, "fence failed_index must be none");
  ExpectInvalid(Fence(Status::Error, AckDurability::Applied, 3, 0),
                "non-sentinel failed_sequence must be nonzero");
  ExpectInvalid(Fence(Status::Error, AckDurability::Applied, 3, 4),
                "failed_sequence must not exceed through_sequence");
  ExpectInvalid(Fence(Status::Error, AckDurability::Applied, 0, 1),
                "failed_sequence cannot name an entry of an empty prefix");
}

}  // namespace
