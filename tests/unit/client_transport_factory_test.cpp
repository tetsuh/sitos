// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <optional>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "sitos/transport.hpp"

namespace sitos {
namespace {

#if defined(SITOS_WITH_ZENOH)
TEST(ClientTransportFactoryTest, RejectsEmptyAndMalformedConfiguration) {
  const auto empty = OpenZenohTransport(std::string_view{});
  ASSERT_FALSE(empty.IsOk());
  EXPECT_EQ(empty.StatusCode(), Status::InvalidArgument);

  const auto malformed = OpenZenohTransport(std::string_view{"not JSON5"});
  ASSERT_FALSE(malformed.IsOk());
  EXPECT_EQ(malformed.StatusCode(), Status::InvalidArgument);
}


TEST(ClientTransportFactoryTest, OpensDefaultAndCompleteJson5Configuration) {
  auto default_transport = OpenZenohTransport();
  ASSERT_TRUE(default_transport.IsOk()) << default_transport.Message();

  auto configured_transport = OpenZenohTransport(std::string_view{"{mode: 'peer'}"});
  ASSERT_TRUE(configured_transport.IsOk()) << configured_transport.Message();

  EXPECT_NE(MakeZenohTransport(), nullptr);
}
#else
TEST(ClientTransportFactoryTest, ReportsZenohDisabled) {
  const auto result = OpenZenohTransport();
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_EQ(result.Error(), std::make_error_code(std::errc::operation_not_supported));
  EXPECT_EQ(MakeZenohTransport(), nullptr);
}
#endif

}  // namespace
}  // namespace sitos
