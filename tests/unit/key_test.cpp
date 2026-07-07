// Copyright 2026 Tetsuya Hayashi
// SPDX-License-Identifier: Apache-2.0

#include <sitos/key.hpp>

#include <gtest/gtest.h>

namespace sitos {
namespace {

TEST(KeyValidationTest, ValidSingleChunkKey) {
  EXPECT_TRUE(IsValidKey("fov"));
  EXPECT_TRUE(IsValidKey("recon"));
  EXPECT_TRUE(IsValidKey("a"));
  EXPECT_TRUE(IsValidKey("A"));
  EXPECT_TRUE(IsValidKey("key_1"));
  EXPECT_TRUE(IsValidKey("key-1"));
  EXPECT_TRUE(IsValidKey("key.1"));
}

TEST(KeyValidationTest, ValidMultiChunkKey) {
  EXPECT_TRUE(IsValidKey("recon/fov"));
  EXPECT_TRUE(IsValidKey("a/b/c"));
  EXPECT_TRUE(IsValidKey("recon/fov/kernel"));
}

TEST(KeyValidationTest, InvalidEmptyKey) {
  EXPECT_FALSE(IsValidKey(""));
}

TEST(KeyValidationTest, InvalidLeadingOrTrailingSlash) {
  EXPECT_FALSE(IsValidKey("/fov"));
  EXPECT_FALSE(IsValidKey("fov/"));
  EXPECT_FALSE(IsValidKey("/recon/fov/"));
}

TEST(KeyValidationTest, InvalidEmptyChunk) {
  EXPECT_FALSE(IsValidKey("recon//fov"));
  EXPECT_FALSE(IsValidKey("/"));
  EXPECT_FALSE(IsValidKey("a//"));
  EXPECT_FALSE(IsValidKey("//a"));
}

TEST(KeyValidationTest, InvalidReservedCharacters) {
  EXPECT_FALSE(IsValidKey("recon*fov"));
  EXPECT_FALSE(IsValidKey("recon$fov"));
  EXPECT_FALSE(IsValidKey("recon?fov"));
  EXPECT_FALSE(IsValidKey("recon#fov"));
  EXPECT_FALSE(IsValidKey("recon@fov"));
}

TEST(KeyValidationTest, InvalidWhitespace) {
  EXPECT_FALSE(IsValidKey("recon fov"));
  EXPECT_FALSE(IsValidKey(" recon"));
  EXPECT_FALSE(IsValidKey("recon\t"));
  EXPECT_FALSE(IsValidKey("recon\nfov"));
}

TEST(KeyValidationTest, CaseSensitive) {
  EXPECT_TRUE(IsValidKey("Recon"));
  EXPECT_TRUE(IsValidKey("FOV"));
}

TEST(SessionIdValidationTest, ValidSessionId) {
  EXPECT_TRUE(IsValidSessionId("session-1"));
  EXPECT_TRUE(IsValidSessionId("abc123"));
  EXPECT_TRUE(IsValidSessionId("ABC_123-xyz"));
  EXPECT_TRUE(IsValidSessionId("a"));
}

TEST(SessionIdValidationTest, InvalidSessionId) {
  EXPECT_FALSE(IsValidSessionId(""));
  EXPECT_FALSE(IsValidSessionId("session/1"));
  EXPECT_FALSE(IsValidSessionId("session*1"));
  EXPECT_FALSE(IsValidSessionId("session 1"));
  EXPECT_FALSE(IsValidSessionId("session.1@"));
}

TEST(PrefixValidationTest, ValidPrefix) {
  EXPECT_TRUE(IsValidPrefix("sitos"));
  EXPECT_TRUE(IsValidPrefix("my/app"));
  EXPECT_TRUE(IsValidPrefix("a/b/c"));
}

TEST(PrefixValidationTest, InvalidPrefix) {
  EXPECT_FALSE(IsValidPrefix(""));
  EXPECT_FALSE(IsValidPrefix("/sitos"));
  EXPECT_FALSE(IsValidPrefix("sitos/"));
  EXPECT_FALSE(IsValidPrefix("sitos//app"));
  EXPECT_FALSE(IsValidPrefix("sitos app"));
}

TEST(ScopeParserTest, ParsesBaseScope) {
  auto scope = ParseScope("base");
  ASSERT_TRUE(scope.has_value());
  EXPECT_EQ(scope->kind, ScopeKind::Base);
  EXPECT_TRUE(scope->sid.empty());
}

TEST(ScopeParserTest, ParsesSessionScope) {
  auto scope = ParseScope("session/abc-123");
  ASSERT_TRUE(scope.has_value());
  EXPECT_EQ(scope->kind, ScopeKind::Session);
  EXPECT_EQ(scope->sid, "abc-123");
}

TEST(ScopeParserTest, ParsesSnapScope) {
  auto scope = ParseScope("snap/xyz_1");
  ASSERT_TRUE(scope.has_value());
  EXPECT_EQ(scope->kind, ScopeKind::Snap);
  EXPECT_EQ(scope->sid, "xyz_1");
}

TEST(ScopeParserTest, RejectsInvalidScope) {
  EXPECT_FALSE(ParseScope(""));
  EXPECT_FALSE(ParseScope("base/"));
  EXPECT_FALSE(ParseScope("session"));
  EXPECT_FALSE(ParseScope("session/"));
  EXPECT_FALSE(ParseScope("session/a/b"));
  EXPECT_FALSE(ParseScope("snap"));
  EXPECT_FALSE(ParseScope("snap/"));
  EXPECT_FALSE(ParseScope("snap/a/b"));
  EXPECT_FALSE(ParseScope("invalid/scope"));
  EXPECT_FALSE(ParseScope("session/a b"));
  EXPECT_FALSE(ParseScope("session/a*b"));
}

TEST(KeyBuilderTest, BuildsBaseKey) {
  auto key = BuildKey("sitos", "base", "recon/fov");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/base/recon/fov");
}

TEST(KeyBuilderTest, BuildsSessionKey) {
  auto key = BuildKey("sitos", "session/abc-123", "recon/fov");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/session/abc-123/recon/fov");
}

TEST(KeyBuilderTest, BuildsSnapKey) {
  auto key = BuildKey("sitos", "snap/abc-123", "recon/fov");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/snap/abc-123/recon/fov");
}

TEST(KeyBuilderTest, BuildsWithCustomPrefix) {
  auto key = BuildKey("my/app", "base", "recon/fov");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "my/app/base/recon/fov");
}

TEST(KeyBuilderTest, RejectsInvalidComponents) {
  EXPECT_FALSE(BuildKey("", "base", "recon/fov"));
  EXPECT_FALSE(BuildKey("sitos", "invalid", "recon/fov"));
  EXPECT_FALSE(BuildKey("sitos", "base", "recon*fov"));
  EXPECT_FALSE(BuildKey("sitos", "session/bad sid", "recon/fov"));
  EXPECT_FALSE(BuildKey("sitos prefix", "base", "recon/fov"));
}

TEST(KeyTest, InvalidKeysAreRejected) {
  // Required AC test name from docs/06_build_test_packaging.md §4.1.
  EXPECT_FALSE(IsValidKey(""));
  EXPECT_FALSE(IsValidKey("/foo"));
  EXPECT_FALSE(IsValidKey("foo/"));
  EXPECT_FALSE(IsValidKey("foo//bar"));
  EXPECT_FALSE(IsValidKey("foo*bar"));
  EXPECT_FALSE(IsValidKey("foo$bar"));
  EXPECT_FALSE(IsValidKey("foo?bar"));
  EXPECT_FALSE(IsValidKey("foo#bar"));
  EXPECT_FALSE(IsValidKey("foo@bar"));
  EXPECT_FALSE(IsValidKey("foo bar"));
  EXPECT_FALSE(IsValidSessionId(""));
  EXPECT_FALSE(IsValidSessionId("bad/sid"));
  EXPECT_FALSE(IsValidSessionId("bad sid"));
  EXPECT_FALSE(ParseScope("session/bad sid"));
  EXPECT_FALSE(BuildKey("sitos", "base", "bad*key"));
}

}  // namespace
}  // namespace sitos
