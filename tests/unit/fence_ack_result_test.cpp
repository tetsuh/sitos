// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "fence_test_support.hpp"

namespace {

using sitos::AckDurability;
using sitos::AckOperationKind;
using sitos::AckResultV1;
using sitos::Status;
using sitos::fence_test::FenceResult;

TEST(FenceAckResultTest, GoldenFailureMatrixRows) {
  struct Row {
    std::string_view name;
    std::string_view fixture;
    AckResultV1 result;
  };
  const std::array rows{
      Row{"applied success", "ack_applied_success.hex",
          FenceResult(Status::Ok, AckDurability::Applied, 7)},
      Row{"synced success", "ack_synced_success.hex",
          FenceResult(Status::Ok, AckDurability::Synced, 7)},
      Row{"empty success", "ack_empty_success.hex",
          FenceResult(Status::Ok, AckDurability::Applied, 0)},
      Row{"empty poison", "ack_empty_poison.hex",
          FenceResult(Status::OutcomeUnknown, AckDurability::Applied, 0)},
      Row{"overtake", "ack_overtake.hex",
          FenceResult(Status::OutcomeUnknown, AckDurability::Applied, 7)},
      Row{"missing session", "ack_missing_session.hex",
          FenceResult(Status::NotFound, AckDurability::Applied, 7)},
      Row{"closing session", "ack_closing_session.hex",
          FenceResult(Status::InvalidArgument, AckDurability::Applied, 7)},
      Row{"malformed global", "ack_malformed_global.hex",
          FenceResult(Status::Error, AckDurability::Applied, 7)},
      Row{"barrier error", "ack_barrier_error.hex",
          FenceResult(Status::Error, AckDurability::Synced, 7)},
      Row{"stale sequence", "ack_stale_sequence.hex",
          FenceResult(Status::Error, AckDurability::Applied, 7, 4)},
      Row{"gap sequence", "ack_gap_sequence.hex",
          FenceResult(Status::OutcomeUnknown, AckDurability::Applied, 7, 4)},
      Row{"write-once sequence", "ack_write_once_sequence.hex",
          FenceResult(Status::InvalidArgument, AckDurability::Applied, 7, 4)},
  };

  for (const auto& row : rows) {
    SCOPED_TRACE(row.name);
    EXPECT_TRUE(sitos::ValidateAckResult(row.result).IsOk());
    const auto encoded = sitos::EncodeAckResult(row.result);
    ASSERT_TRUE(encoded.IsOk());
    EXPECT_EQ(encoded.Value(), sitos::fence_test::ReadHexFixture(row.fixture));
    const auto decoded = sitos::DecodeAckResult(encoded.Value());
    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value(), row.result);
  }

  const auto valid = FenceResult(Status::Error, AckDurability::Applied, 7, 4);
  const auto accepted_by_adr_0028 = FenceResult(Status::Ok, AckDurability::Applied, 7, 4);
  const std::array invalid{
      AckResultV1{AckOperationKind::Put, Status::Ok, AckDurability::Applied, 0,
                  sitos::kAckNoFailedIndex, 7, sitos::kAckNoFailedSequence, ""},
      AckResultV1{AckOperationKind::Fence, Status::Ok, AckDurability::Applied, 1,
                  sitos::kAckNoFailedIndex, 7, sitos::kAckNoFailedSequence, ""},
      AckResultV1{AckOperationKind::Fence, Status::Ok, AckDurability::Applied, 0, 0, 7,
                  sitos::kAckNoFailedSequence, ""},
      AckResultV1{AckOperationKind::Fence, Status::Error, AckDurability::Applied, 0,
                  sitos::kAckNoFailedIndex, 7, 0, ""},
      AckResultV1{AckOperationKind::Fence, Status::OutcomeUnknown, AckDurability::Applied, 0,
                  sitos::kAckNoFailedIndex, 7, 8, ""},
      AckResultV1{AckOperationKind::Fence, Status::Error, static_cast<AckDurability>(0), 0,
                  sitos::kAckNoFailedIndex, 7, sitos::kAckNoFailedSequence, ""},
      AckResultV1{static_cast<AckOperationKind>(0), Status::Error, AckDurability::Applied, 0,
                  sitos::kAckNoFailedIndex, 7, sitos::kAckNoFailedSequence, ""},
  };
  EXPECT_TRUE(sitos::ValidateAckResult(valid).IsOk());
  EXPECT_TRUE(sitos::ValidateAckResult(accepted_by_adr_0028).IsOk())
      << "Issue #158 may not narrow the reused ADR-0028 result decoder";
  for (const auto& result : invalid) {
    EXPECT_FALSE(sitos::ValidateAckResult(result).IsOk());
  }
}

}  // namespace
