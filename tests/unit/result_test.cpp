// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "sitos/result.hpp"
#include "sitos/status.hpp"

namespace sitos {
namespace {

TEST(ResultTest, PreservesValueAndErrorStates) {
  auto value = Result<int>::Ok(42);
  EXPECT_TRUE(value.IsOk());
  EXPECT_EQ(value.StatusCode(), Status::Ok);
  EXPECT_TRUE(value.Message().empty());
  EXPECT_EQ(value.Value(), 42);

  auto error = Result<int>::Err(Status::NotFound, "missing");
  EXPECT_FALSE(error.IsOk());
  EXPECT_EQ(error.StatusCode(), Status::NotFound);
  EXPECT_EQ(error.Message(), "missing");
  EXPECT_EQ(error.Error().category(), StatusErrorCategory());
}

TEST(ResultTest, PreservesNativeCause) {
  const auto cause = std::make_error_code(std::errc::file_exists);
  auto result = Result<int>::Err(Status::Error, "failed", cause);
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_EQ(result.Message(), "failed");
  EXPECT_EQ(result.Error(), cause);
}

TEST(ResultTest, ErrFromPreservesFailureAcrossValueTypes) {
  auto source = Result<std::unique_ptr<int>>::Err(Status::Disconnected, "offline");
  auto result = Result<void>::ErrFrom(source);
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Disconnected);
  EXPECT_EQ(result.Message(), "offline");
  EXPECT_EQ(result.Error(), source.Error());
}

TEST(ResultTest, SupportsMoveOnlyValues) {
  auto result = Result<std::unique_ptr<int>>::Ok(std::make_unique<int>(7));
  auto value = std::move(result).Value();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 7);
}

TEST(ResultTest, MapsPortableLegacyErrors) {
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::invalid_argument)).StatusCode(),
            Status::InvalidArgument);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::timed_out)).StatusCode(),
            Status::Timeout);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::not_connected)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::connection_aborted)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::connection_refused)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::connection_reset)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::network_down)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::network_reset)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::network_unreachable)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::host_unreachable)).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(Result<void>::Err(std::make_error_code(std::errc::broken_pipe)).StatusCode(),
            Status::Disconnected);
}

TEST(ResultTest, UnknownLegacyErrorsRemainErrors) {
  const auto cause = std::make_error_code(std::errc::file_exists);
  auto result = Result<void>::Err(cause);
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_EQ(result.Error(), cause);

  const auto no_such_file = std::make_error_code(std::errc::no_such_file_or_directory);
  result = Result<void>::Err(no_such_file);
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_EQ(result.Error(), no_such_file);
}

TEST(ResultTest, VoidSuccessHasNoError) {
  auto result = Result<void>::Ok();
  EXPECT_TRUE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Ok);
  EXPECT_TRUE(result.Message().empty());
}

#if defined(NDEBUG)
TEST(ResultTest, ZeroErrorCodeIsNormalized) {
  auto result = Result<void>::Err(std::error_code{});
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_NE(result.Error().value(), 0);
}
#endif

TEST(StatusTest, ErrorCodeUsesStableCategory) {
  const auto error = MakeErrorCode(Status::Timeout);
  EXPECT_NE(error.value(), 0);
  EXPECT_EQ(&error.category(), &StatusErrorCategory());
}

}  // namespace
}  // namespace sitos
