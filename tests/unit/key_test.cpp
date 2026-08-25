// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <sitos/key.hpp>
#include <string>
#include <string_view>

namespace sitos {
namespace {

static_assert(static_cast<int>(KeyKind::Buffer) == 5,
              "Issue #158 must preserve the public KeyKind numbering");

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

TEST(KeyValidationTest, InvalidEmptyKey) { EXPECT_FALSE(IsValidKey("")); }

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

// --- :batch builders (docs/03 §1.1) ---

TEST(BatchKeyBuilderTest, BuildsBaseBatch) {
  auto key = BuildBatchKey("sitos", "base");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/base/:batch");
}

TEST(BatchKeyBuilderTest, BuildsSessionBatch) {
  auto key = BuildBatchKey("sitos", "session/abc-123");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/session/abc-123/:batch");
}

TEST(BatchKeyBuilderTest, BuildsBatchWithCustomPrefix) {
  auto key = BuildBatchKey("my/app", "base");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "my/app/base/:batch");
}

TEST(BatchKeyBuilderTest, RejectsSnapBatch) {
  // Snap batch is not defined by the wire protocol.
  EXPECT_FALSE(BuildBatchKey("sitos", "snap/abc-123"));
}

TEST(BatchKeyBuilderTest, RejectsInvalidBatchScope) {
  EXPECT_FALSE(BuildBatchKey("", "base"));
  EXPECT_FALSE(BuildBatchKey("sitos", "invalid"));
  EXPECT_FALSE(BuildBatchKey("sitos", "session/bad sid"));
  EXPECT_FALSE(BuildBatchKey("sitos prefix", "base"));
}

TEST(BatchKeyBuilderTest, UserKeyCannotSmuggleBatch) {
  // ':' is outside the user-key grammar, so batch paths can only be produced
  // through BuildBatchKey.
  EXPECT_FALSE(BuildKey("sitos", "base", ":batch"));
}

// --- meta builders (docs/03 §1.1) ---

TEST(MetaKeyBuilderTest, BuildsMetaSession) {
  auto key = BuildMetaSessionKey("sitos", "abc-123");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/meta/session/abc-123");
}

TEST(MetaKeyBuilderTest, BuildsMetaAck) {
  auto key = BuildMetaAckKey("sitos", "550e8400-e29b-41d4-a716-446655440000");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "sitos/meta/ack/550e8400-e29b-41d4-a716-446655440000");
}

TEST(MetaKeyBuilderTest, RejectsInvalidMeta) {
  EXPECT_FALSE(BuildMetaSessionKey("", "abc-123"));
  EXPECT_FALSE(BuildMetaSessionKey("sitos", ""));
  EXPECT_FALSE(BuildMetaSessionKey("sitos", "bad/sid"));
  EXPECT_FALSE(BuildMetaSessionKey("sitos", "bad sid"));
  EXPECT_FALSE(BuildMetaAckKey("", "uuid"));
  EXPECT_FALSE(BuildMetaAckKey("sitos", "bad/uuid"));
}

// --- incoming-key parser (docs/03 §1.1, inverse of Build*) ---

TEST(ParseKeyTest, ParsesBaseKey) {
  auto p = ParseKey("sitos", "sitos/base/recon/fov");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Base);
  EXPECT_TRUE(p->sid.empty());
  EXPECT_TRUE(p->uuid.empty());
  EXPECT_EQ(p->relative_key, "recon/fov");
  EXPECT_FALSE(p->is_batch);
}

TEST(ParseKeyTest, ParsesBaseBatchKey) {
  auto p = ParseKey("sitos", "sitos/base/:batch");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Base);
  EXPECT_TRUE(p->relative_key.empty());
  EXPECT_TRUE(p->is_batch);
}

TEST(ParseKeyTest, ParsesSessionKey) {
  auto p = ParseKey("sitos", "sitos/session/abc-123/recon/fov");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Session);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_EQ(p->relative_key, "recon/fov");
  EXPECT_FALSE(p->is_batch);
}

TEST(ParseKeyTest, ParsesSessionBatchKey) {
  auto p = ParseKey("sitos", "sitos/session/abc-123/:batch");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Session);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_TRUE(p->relative_key.empty());
  EXPECT_TRUE(p->is_batch);
}

TEST(ParseKeyTest, ParsesSnapshotKey) {
  auto p = ParseKey("sitos", "sitos/snap/abc-123/recon/fov");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Snapshot);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_EQ(p->relative_key, "recon/fov");
  EXPECT_FALSE(p->is_batch);
}

TEST(ParseKeyTest, ParsesMetaSession) {
  auto p = ParseKey("sitos", "sitos/meta/session/abc-123");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::MetaSession);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_TRUE(p->relative_key.empty());
  EXPECT_FALSE(p->is_batch);
}

TEST(ParseKeyTest, ParsesMetaAck) {
  auto p = ParseKey("sitos", "sitos/meta/ack/550e8400-e29b-41d4-a716-446655440000");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::MetaAck);
  EXPECT_EQ(p->uuid, "550e8400-e29b-41d4-a716-446655440000");
  EXPECT_TRUE(p->relative_key.empty());
  EXPECT_FALSE(p->is_batch);
}

TEST(ParseKeyTest, ParsesWithCustomPrefix) {
  auto p = ParseKey("my/app", "my/app/base/recon/fov");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Base);
  EXPECT_EQ(p->relative_key, "recon/fov");
}

TEST(ParseKeyTest, RejectsPrefixMismatchAndBarePrefix) {
  EXPECT_FALSE(ParseKey("sitos", "other/base/recon/fov"));  // wrong prefix
  EXPECT_FALSE(ParseKey("sitos", "sitos"));                 // prefix only
  EXPECT_FALSE(ParseKey("sitos", "sitos/base"));            // base with no key
  EXPECT_FALSE(ParseKey("sitos", "sitos/session/abc"));     // session/sid with no key
  EXPECT_FALSE(ParseKey("sitos", "sitos/snap/abc"));        // snap/sid with no key
}

TEST(ParseKeyTest, RejectsInvalidKinds) {
  EXPECT_FALSE(ParseKey("sitos", "sitos/unknown/recon/fov"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/meta/unknown/abc"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/meta/session/bad sid"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/meta/ack/bad/uuid"));
}

TEST(ParseKeyTest, RejectsUnsupportedBatchForms) {
  // snap has no :batch path, and retired wire candidates remain invalid.
  EXPECT_FALSE(ParseKey("sitos", "sitos/snap/abc-123/:batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/base/$batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/base/@batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/base/~batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/session/abc-123/$batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/session/abc-123/@batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/session/abc-123/~batch"));
}

TEST(ParseKeyTest, RejectsInvalidRelativeKey) {
  EXPECT_FALSE(ParseKey("sitos", "sitos/base/recon*fov"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/base/recon//fov"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/session/abc-123/recon$fov"));
}

// AC #2: round-trip build -> parse returns the original components.
TEST(KeyRoundTripTest, BuildThenParseRecoversBase) {
  auto built = BuildKey("sitos", "base", "recon/fov");
  ASSERT_TRUE(built.has_value());
  auto p = ParseKey("sitos", *built);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Base);
  EXPECT_EQ(p->relative_key, "recon/fov");
  EXPECT_FALSE(p->is_batch);
}

TEST(KeyRoundTripTest, BuildThenParseRecoversSession) {
  auto built = BuildKey("sitos", "session/abc-123", "recon/fov");
  ASSERT_TRUE(built.has_value());
  auto p = ParseKey("sitos", *built);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Session);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_EQ(p->relative_key, "recon/fov");
}

TEST(KeyRoundTripTest, BuildThenParseRecoversSnapshot) {
  auto built = BuildKey("sitos", "snap/abc-123", "recon/fov");
  ASSERT_TRUE(built.has_value());
  auto p = ParseKey("sitos", *built);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Snapshot);
  EXPECT_EQ(p->sid, "abc-123");
  EXPECT_EQ(p->relative_key, "recon/fov");
  EXPECT_FALSE(p->is_batch);
}

TEST(KeyRoundTripTest, BuildThenParseRecoversBatch) {
  auto built = BuildBatchKey("sitos", "base");
  ASSERT_TRUE(built.has_value());
  auto p = ParseKey("sitos", *built);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::Base);
  EXPECT_TRUE(p->is_batch);

  auto built2 = BuildBatchKey("sitos", "session/abc-123");
  ASSERT_TRUE(built2.has_value());
  auto p2 = ParseKey("sitos", *built2);
  ASSERT_TRUE(p2.has_value());
  EXPECT_EQ(p2->kind, KeyKind::Session);
  EXPECT_EQ(p2->sid, "abc-123");
  EXPECT_TRUE(p2->is_batch);
}

TEST(KeyRoundTripTest, BuildThenParseRecoversMeta) {
  auto built = BuildMetaSessionKey("sitos", "abc-123");
  ASSERT_TRUE(built.has_value());
  auto p = ParseKey("sitos", *built);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->kind, KeyKind::MetaSession);
  EXPECT_EQ(p->sid, "abc-123");

  const std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
  auto built2 = BuildMetaAckKey("sitos", uuid);
  ASSERT_TRUE(built2.has_value());
  auto p2 = ParseKey("sitos", *built2);
  ASSERT_TRUE(p2.has_value());
  EXPECT_EQ(p2->kind, KeyKind::MetaAck);
  EXPECT_EQ(p2->uuid, uuid);
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

TEST(BufferKeyTest, BufferRoutesRoundTrip) {
  const auto durable = BuildBufferKey("my/app", "session-1", BufferClass::Durable, "durable/part");
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(*durable, "my/app/buffers/session-1/durable/durable/part");
  const auto durable_parsed = ParseKey("my/app", *durable);
  ASSERT_TRUE(durable_parsed.has_value());
  EXPECT_EQ(durable_parsed->kind, KeyKind::Buffer);
  EXPECT_EQ(durable_parsed->sid, "session-1");
  ASSERT_TRUE(durable_parsed->buffer_class.has_value());
  EXPECT_EQ(*durable_parsed->buffer_class, BufferClass::Durable);
  EXPECT_EQ(durable_parsed->relative_key, "durable/part");
  EXPECT_TRUE(durable_parsed->uuid.empty());
  EXPECT_FALSE(durable_parsed->is_batch);

  const auto ephemeral =
      BuildBufferKey("my/app", "session-1", BufferClass::Ephemeral, "ephemeral/part");
  ASSERT_TRUE(ephemeral.has_value());
  EXPECT_EQ(*ephemeral, "my/app/buffers/session-1/ephemeral/ephemeral/part");
  const auto ephemeral_parsed = ParseKey("my/app", *ephemeral);
  ASSERT_TRUE(ephemeral_parsed.has_value());
  EXPECT_EQ(ephemeral_parsed->kind, KeyKind::Buffer);
  EXPECT_EQ(ephemeral_parsed->sid, "session-1");
  ASSERT_TRUE(ephemeral_parsed->buffer_class.has_value());
  EXPECT_EQ(*ephemeral_parsed->buffer_class, BufferClass::Ephemeral);
  EXPECT_EQ(ephemeral_parsed->relative_key, "ephemeral/part");
  EXPECT_TRUE(ephemeral_parsed->uuid.empty());
  EXPECT_FALSE(ephemeral_parsed->is_batch);

  const auto base = ParseKey("my/app", "my/app/base/key");
  ASSERT_TRUE(base.has_value());
  EXPECT_FALSE(base->buffer_class.has_value());
  const auto session = ParseKey("my/app", "my/app/session/session-1/key");
  ASSERT_TRUE(session.has_value());
  EXPECT_FALSE(session->buffer_class.has_value());
  const auto snapshot = ParseKey("my/app", "my/app/snap/session-1/key");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_FALSE(snapshot->buffer_class.has_value());
  const auto meta_session = ParseKey("my/app", "my/app/meta/session/session-1");
  ASSERT_TRUE(meta_session.has_value());
  EXPECT_FALSE(meta_session->buffer_class.has_value());
  const auto meta_ack = ParseKey("my/app", "my/app/meta/ack/ack-1");
  ASSERT_TRUE(meta_ack.has_value());
  EXPECT_FALSE(meta_ack->buffer_class.has_value());
}

TEST(BufferKeyTest, InvalidBufferRoutesAreRejected) {
  EXPECT_FALSE(BuildBufferKey("", "session-1", BufferClass::Durable, "key"));
  EXPECT_FALSE(BuildBufferKey("sitos", "", BufferClass::Durable, "key"));
  EXPECT_FALSE(BuildBufferKey("sitos", "session-1", BufferClass::Durable, ""));
  EXPECT_FALSE(BuildBufferKey("sitos", "bad/sid", BufferClass::Durable, "key"));
  EXPECT_FALSE(BuildBufferKey("sitos", "session-1", BufferClass::Durable, "bad*key"));
  EXPECT_FALSE(BuildBufferKey("sitos", "session-1", BufferClass::Ephemeral, "key/"));
  EXPECT_FALSE(BuildBufferKey("sitos", "session-1", static_cast<BufferClass>(99), "key"));

  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/unknown/key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers//durable/key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1//key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session 1/durable/key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/bad*key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable//key"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/:batch"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/:fence"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/:control"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/key/:fence"));
  EXPECT_FALSE(ParseKey("sitos", "sitos/buffers/session-1/durable/key/extra/"));
}

TEST(BufferKeyTest, BufferClassIsReservedOnlyInTheClassPosition) {
  const auto durable =
      BuildBufferKey("sitos", "session-1", BufferClass::Durable, "nested/durable/ephemeral");
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(*durable, "sitos/buffers/session-1/durable/nested/durable/ephemeral");
  const auto durable_parsed = ParseKey("sitos", *durable);
  ASSERT_TRUE(durable_parsed.has_value());
  EXPECT_EQ(durable_parsed->kind, KeyKind::Buffer);
  EXPECT_EQ(durable_parsed->sid, "session-1");
  EXPECT_TRUE(durable_parsed->uuid.empty());
  EXPECT_EQ(durable_parsed->relative_key, "nested/durable/ephemeral");
  EXPECT_FALSE(durable_parsed->is_batch);
  ASSERT_TRUE(durable_parsed->buffer_class.has_value());
  EXPECT_EQ(*durable_parsed->buffer_class, BufferClass::Durable);

  const auto ephemeral =
      BuildBufferKey("sitos", "session-1", BufferClass::Ephemeral, "nested/durable/ephemeral");
  ASSERT_TRUE(ephemeral.has_value());
  EXPECT_EQ(*ephemeral, "sitos/buffers/session-1/ephemeral/nested/durable/ephemeral");
  const auto ephemeral_parsed = ParseKey("sitos", *ephemeral);
  ASSERT_TRUE(ephemeral_parsed.has_value());
  EXPECT_EQ(ephemeral_parsed->kind, KeyKind::Buffer);
  EXPECT_EQ(ephemeral_parsed->sid, "session-1");
  EXPECT_TRUE(ephemeral_parsed->uuid.empty());
  EXPECT_EQ(ephemeral_parsed->relative_key, "nested/durable/ephemeral");
  EXPECT_FALSE(ephemeral_parsed->is_batch);
  ASSERT_TRUE(ephemeral_parsed->buffer_class.has_value());
  EXPECT_EQ(*ephemeral_parsed->buffer_class, BufferClass::Ephemeral);
}

}  // namespace
}  // namespace sitos
