// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <optional>

#include <gtest/gtest.h>

#include "sitos/client_config.hpp"

namespace sitos {
namespace {

TEST(ClientConfigTest, HasRequiredDefaults) {
  const ClientConfig config;
  EXPECT_EQ(config.prefix, "sitos");
  EXPECT_FALSE(config.zenoh_config_json.has_value());
  EXPECT_EQ(config.query_timeout, std::chrono::milliseconds(5000));
  EXPECT_TRUE(ValidateClientConfig(config).IsOk());
}

TEST(ClientConfigTest, RejectsInvalidPrefix) {
  ClientConfig config;
  config.prefix = "sitos/**";
  const auto result = ValidateClientConfig(config);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::InvalidKey);
}

TEST(ClientConfigTest, RejectsNonpositiveTimeout) {
  ClientConfig config;
  config.query_timeout = std::chrono::milliseconds::zero();
  auto result = ValidateClientConfig(config);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::InvalidArgument);

  config.query_timeout = std::chrono::milliseconds(-1);
  result = ValidateClientConfig(config);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::InvalidArgument);
}

TEST(ClientConfigTest, RejectsEmptyJsonButDoesNotParseNonemptyJson) {
  ClientConfig config;
  config.zenoh_config_json = std::string{};
  auto result = ValidateClientConfig(config);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::InvalidArgument);

  config.zenoh_config_json = "not JSON5";
  EXPECT_TRUE(ValidateClientConfig(config).IsOk());
}

}  // namespace
}  // namespace sitos
