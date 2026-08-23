// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the ADR-0028 StorageNode token registry: the bounded
// Processing/Completed state machine, fingerprint collision detection, the
// 4096-entry completion-order ring, and the dependency-free SHA-256 used for
// operation fingerprints.

#include "ack_registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.hpp"
#include "sitos/ack.hpp"

namespace {

using sitos::AckDurability;
using sitos::AckFingerprint;
using sitos::AckOperationKind;
using sitos::AckRegistry;
using sitos::AckResultV1;
using sitos::AckToken;
using sitos::ComputeAckFingerprint;
using sitos::kAckNoFailedIndex;
using sitos::kAckNoFailedSequence;
using sitos::Status;
using Outcome = AckRegistry::ClaimOutcome;

std::string Hex(std::span<const std::byte> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::byte b : bytes) {
    out.push_back(kDigits[static_cast<std::uint8_t>(b) >> 4]);
    out.push_back(kDigits[static_cast<std::uint8_t>(b) & 0x0F]);
  }
  return out;
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> out;
  for (unsigned char c : text) out.push_back(std::byte{c});
  return out;
}

AckToken Token(std::uint8_t seed) {
  AckToken token;
  token.bytes.fill(std::byte{seed});
  token.bytes[6] = std::byte{0x40};
  token.bytes[8] = std::byte{0x80};
  return token;
}

AckFingerprint Fingerprint(std::string_view payload = "value") {
  return ComputeAckFingerprint(AckOperationKind::Put, "sitos/base/key", "sitos.v1", Bytes(payload));
}

AckResultV1 PutOk() {
  return AckResultV1{AckOperationKind::Put, Status::Ok, AckDurability::Applied, 1,
                     kAckNoFailedIndex,     0,          kAckNoFailedSequence,   ""};
}

AckResultV1 PutError() {
  return AckResultV1{AckOperationKind::Put,
                     Status::Error,
                     AckDurability::Applied,
                     0,
                     0,
                     0,
                     kAckNoFailedSequence,
                     ""};
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4 vectors)
// ---------------------------------------------------------------------------

TEST(Sha256Test, MatchesKnownVectors) {
  EXPECT_EQ(Hex(sitos::Sha256({})),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Hex(sitos::Sha256(Bytes("abc"))),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Hex(sitos::Sha256(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // 1,000,000 x 'a' exercises many full blocks.
  const std::vector<std::byte> million(1000000, std::byte{'a'});
  EXPECT_EQ(Hex(sitos::Sha256(million)),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // 55/56/64-byte inputs straddle the one-block/two-block padding boundary and must
  // all produce distinct digests.
  const std::string d55 = Hex(sitos::Sha256(std::vector<std::byte>(55, std::byte{'x'})));
  const std::string d56 = Hex(sitos::Sha256(std::vector<std::byte>(56, std::byte{'x'})));
  const std::string d64 = Hex(sitos::Sha256(std::vector<std::byte>(64, std::byte{'x'})));
  EXPECT_NE(d55, d56);
  EXPECT_NE(d56, d64);
  EXPECT_NE(d55, d64);
  EXPECT_EQ(d55, Hex(sitos::Sha256(std::vector<std::byte>(55, std::byte{'x'}))))
      << "deterministic";
}

// ---------------------------------------------------------------------------
// Fingerprint
// ---------------------------------------------------------------------------

TEST(AckFingerprintTest, DistinguishesEveryCoveredField) {
  const auto base = Fingerprint();
  EXPECT_EQ(base, Fingerprint());
  EXPECT_NE(base, ComputeAckFingerprint(AckOperationKind::Batch, "sitos/base/key", "sitos.v1",
                                        Bytes("value")));
  EXPECT_NE(base, ComputeAckFingerprint(AckOperationKind::Put, "sitos/base/other", "sitos.v1",
                                        Bytes("value")));
  EXPECT_NE(base, ComputeAckFingerprint(AckOperationKind::Put, "sitos/base/key", "zenoh/bytes",
                                        Bytes("value")));
  EXPECT_NE(base, Fingerprint("valu"));
  // Field boundaries are length-delimited: moving bytes between key and payload changes the hash.
  EXPECT_NE(ComputeAckFingerprint(AckOperationKind::Put, "ab", "", Bytes("c")),
            ComputeAckFingerprint(AckOperationKind::Put, "a", "", Bytes("bc")));
}

// ---------------------------------------------------------------------------
// Registry state machine
// ---------------------------------------------------------------------------

TEST(AckRegistryTest, ClaimAdmitsNewTokenAndCompletePublishesImmutableResult) {
  AckRegistry registry;
  const AckToken token = Token(1);
  EXPECT_EQ(registry.Claim(token, Fingerprint(), AckRegistry::kParameterLane), Outcome::Admitted);
  EXPECT_EQ(registry.ProcessingCount(), 1u);
  EXPECT_FALSE(registry.Find(token).has_value()) << "Processing observes no result";

  EXPECT_TRUE(registry.Complete(token, PutOk()));
  EXPECT_EQ(registry.ProcessingCount(), 0u);
  ASSERT_TRUE(registry.Find(token).has_value());
  EXPECT_EQ(*registry.Find(token), PutOk());
  EXPECT_FALSE(registry.Complete(token, PutError())) << "completion happens exactly once";
  EXPECT_EQ(*registry.Find(token), PutOk());
  EXPECT_EQ(registry.Size(), 1u);
}

TEST(AckRegistryTest, DuplicateTokenWithSameFingerprintIsSuppressedInBothStates) {
  AckRegistry registry;
  const AckToken token = Token(2);
  ASSERT_EQ(registry.Claim(token, Fingerprint(), AckRegistry::kParameterLane), Outcome::Admitted);
  EXPECT_EQ(registry.Claim(token, Fingerprint(), AckRegistry::kParameterLane),
            Outcome::DuplicateProcessing);
  ASSERT_TRUE(registry.Complete(token, PutOk()));
  EXPECT_EQ(registry.Claim(token, Fingerprint(), AckRegistry::kParameterLane),
            Outcome::DuplicateCompleted);
  EXPECT_EQ(registry.Size(), 1u);
}

TEST(AckRegistryTest, DifferentFingerprintIsCollisionAndPreservesOriginal) {
  AckRegistry registry;
  const AckToken token = Token(3);
  ASSERT_EQ(registry.Claim(token, Fingerprint("a"), AckRegistry::kParameterLane),
            Outcome::Admitted);
  EXPECT_EQ(registry.Claim(token, Fingerprint("b"), AckRegistry::kParameterLane),
            Outcome::Collision);
  ASSERT_TRUE(registry.Complete(token, PutOk()));
  EXPECT_EQ(registry.Claim(token, Fingerprint("b"), AckRegistry::kParameterLane),
            Outcome::Collision);
  EXPECT_EQ(*registry.Find(token), PutOk());
  EXPECT_EQ(registry.Size(), 1u);
}

TEST(AckRegistryTest, LaneAdmitsOneProcessingTokenAtATime) {
  AckRegistry registry;
  ASSERT_EQ(registry.Claim(Token(4), Fingerprint("a"), AckRegistry::kParameterLane),
            Outcome::Admitted);
  EXPECT_EQ(registry.Claim(Token(5), Fingerprint("b"), AckRegistry::kParameterLane),
            Outcome::LaneBusy);
  EXPECT_FALSE(registry.Find(Token(5)).has_value()) << "LaneBusy does not record the token";
  EXPECT_EQ(registry.Claim(Token(5), Fingerprint("b"), /*lane=*/7), Outcome::Admitted)
      << "other lanes are independent";
  ASSERT_TRUE(registry.Complete(Token(4), PutOk()));
  EXPECT_EQ(registry.Claim(Token(6), Fingerprint("c"), AckRegistry::kParameterLane),
            Outcome::Admitted);
}

TEST(AckRegistryTest, LaneBusyRejectionIsAtomicallyRetained) {
  AckRegistry registry;
  const AckToken owner = Token(6);
  const AckToken rejected = Token(7);
  ASSERT_EQ(registry.Claim(owner, Fingerprint("owner"), AckRegistry::kParameterLane),
            Outcome::Admitted);

  EXPECT_EQ(registry.ClaimOrReject(rejected, Fingerprint("rejected"),
                                   AckRegistry::kParameterLane, PutError()),
            Outcome::LaneBusy);
  EXPECT_EQ(registry.ProcessingCount(), 1u);
  ASSERT_TRUE(registry.Find(rejected).has_value());
  EXPECT_EQ(*registry.Find(rejected), PutError());

  ASSERT_TRUE(registry.Complete(owner, PutOk()));
  EXPECT_EQ(registry.Claim(rejected, Fingerprint("rejected"), AckRegistry::kParameterLane),
            Outcome::DuplicateCompleted)
      << "the rejected token must never become admissible after the lane owner completes";
  EXPECT_EQ(*registry.Find(rejected), PutError());
}

TEST(AckRegistryTest, RecordRejectedStoresCompletedResultWithoutProcessing) {
  AckRegistry registry;
  const AckToken token = Token(8);
  EXPECT_TRUE(registry.RecordRejected(token, Fingerprint(), PutError()));
  EXPECT_EQ(registry.ProcessingCount(), 0u);
  EXPECT_EQ(*registry.Find(token), PutError());
  EXPECT_FALSE(registry.RecordRejected(token, Fingerprint(), PutOk())) << "token already retained";
  EXPECT_EQ(registry.Claim(token, Fingerprint(), AckRegistry::kParameterLane),
            Outcome::DuplicateCompleted);
}

TEST(AckRegistryTest, EvictsCompletedResultsInCompletionOrderAndNeverProcessing) {
  AckRegistry registry(/*capacity=*/3);
  for (std::uint8_t i = 1; i <= 3; ++i) {
    ASSERT_EQ(registry.Claim(Token(i), Fingerprint(), AckRegistry::kParameterLane),
              Outcome::Admitted);
    ASSERT_TRUE(registry.Complete(Token(i), PutOk()));
  }
  ASSERT_EQ(registry.Claim(Token(9), Fingerprint(), AckRegistry::kParameterLane),
            Outcome::Admitted);  // Processing, must survive eviction
  ASSERT_EQ(registry.Claim(Token(4), Fingerprint(), /*lane=*/1), Outcome::Admitted);
  ASSERT_TRUE(registry.Complete(Token(4), PutOk()));

  EXPECT_FALSE(registry.Find(Token(1)).has_value()) << "oldest completed result evicted";
  EXPECT_TRUE(registry.Find(Token(2)).has_value());
  EXPECT_TRUE(registry.Find(Token(3)).has_value());
  EXPECT_TRUE(registry.Find(Token(4)).has_value());
  EXPECT_EQ(registry.ProcessingCount(), 1u);
  // After eviction the token is no longer retained: a resubmission is admitted again.
  EXPECT_EQ(registry.Claim(Token(1), Fingerprint("other"), /*lane=*/2), Outcome::Admitted);
}

TEST(AckRegistryTest, ClearRemovesProcessingAndCompletedState) {
  AckRegistry registry;
  ASSERT_EQ(registry.Claim(Token(1), Fingerprint(), AckRegistry::kParameterLane),
            Outcome::Admitted);
  ASSERT_TRUE(registry.Complete(Token(1), PutOk()));
  ASSERT_EQ(registry.Claim(Token(2), Fingerprint(), AckRegistry::kParameterLane),
            Outcome::Admitted);
  registry.Clear();
  EXPECT_EQ(registry.Size(), 0u);
  EXPECT_EQ(registry.ProcessingCount(), 0u);
  EXPECT_FALSE(registry.Find(Token(1)).has_value());
  EXPECT_EQ(registry.Claim(Token(2), Fingerprint(), AckRegistry::kParameterLane), Outcome::Admitted)
      << "the lane is free again after Clear";
}

}  // namespace
