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

#include <string>

namespace {

TEST(ZenohSmokeTest, OpenAndCloseSession) {
  z_owned_config_t config;
  ASSERT_EQ(z_config_default(&config), Z_OK);

  z_owned_session_t session;
  ASSERT_EQ(z_open(&session, z_move(config), nullptr), Z_OK);

  ASSERT_EQ(z_close(z_session_loan_mut(&session), nullptr), Z_OK);

  z_drop(z_move(session));
}

TEST(ZenohSmokeTest, BytesEncodingUsesCanonicalSitosSchema) {
  z_owned_encoding_t encoding;
  z_encoding_clone(&encoding, z_encoding_zenoh_bytes());
  ASSERT_EQ(z_encoding_set_schema_from_str(z_loan_mut(encoding), "sitos.v1"), Z_OK);

  z_owned_string_t text;
  z_encoding_to_string(z_loan(encoding), &text);
  EXPECT_EQ(std::string(z_string_data(z_loan(text)), z_string_len(z_loan(text))),
            "zenoh/bytes;sitos.v1");

  z_drop(z_move(text));
  z_drop(z_move(encoding));
}

}  // namespace
