// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <system_error>

#include "sitos/rocksdb_engine.hpp"

TEST(RocksDBEngineOffTest, OpenReturnsOperationNotSupported) {
  const auto result = sitos::RocksDBEngine::Open("disabled-db");
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  EXPECT_EQ(result.Error(), std::make_error_code(std::errc::operation_not_supported));
}
