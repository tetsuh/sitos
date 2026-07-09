// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Smoke test: opens and closes a zenoh session to verify the build integration.

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <zenoh.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <gtest/gtest.h>

namespace {

TEST(ZenohSmokeTest, OpenAndCloseSession) {
  z_owned_config_t config;
  ASSERT_EQ(z_config_default(&config), Z_OK);

  z_owned_session_t session;
  ASSERT_EQ(z_open(&session, z_move(config), nullptr), Z_OK);

  ASSERT_EQ(z_close(z_session_loan_mut(&session), nullptr), Z_OK);

  z_drop(z_move(session));
}

}  // namespace
