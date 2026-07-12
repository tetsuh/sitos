// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/storage_node.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sitos/in_memory_engine.hpp"

namespace sitos {
namespace {

class FakeTransport final : public Transport {
 public:
  struct ReplyRecord {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
  };

  Result<void> Put(std::string_view, std::span<const std::byte>, Encoding,
                   PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Result<void> Delete(std::string_view, PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Subscription DeclareSubscriber(std::string_view,
                                 std::function<void(const TransportSample&)>) override {
    return {};
  }

  Queryable DeclareQueryable(std::string_view keyexpr,
                             const std::function<void(TransportQuery&)>& callback) override {
    declared_keyexpr = std::string(keyexpr);
    query_callback = callback;
    return {};
  }

  std::vector<ReplyRecord> Invoke(std::string keyexpr, bool fail_after_first = false) {
    std::vector<ReplyRecord> replies;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, Encoding encoding) {
          replies.push_back({std::string(key), std::vector<std::byte>(payload.begin(), payload.end()),
                             std::move(encoding)});
          if (fail_after_first && replies.size() == 1) {
            return Result<void>::Err(std::make_error_code(std::errc::io_error));
          }
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(keyexpr);
    query_callback(query);
    return replies;
  }

  std::string declared_keyexpr;
  std::function<void(TransportQuery&)> query_callback;
};

TEST(StorageNodeQueryTest, ParsesExactBaseKey) {
  auto parsed = ParseStorageQuery("sitos", "sitos/base/foo/bar");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->is_list);
  EXPECT_EQ(parsed->relative_key, "foo/bar");
}

TEST(StorageNodeQueryTest, ParsesRootBaseListSelector) {
  auto parsed = ParseStorageQuery("sitos", "sitos/base/**");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->is_list);
  EXPECT_TRUE(parsed->relative_key.empty());
}

TEST(StorageNodeQueryTest, ParsesNestedBaseListSelectorAtChunkBoundary) {
  auto parsed = ParseStorageQuery("sitos", "sitos/base/foo/bar/**");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->is_list);
  EXPECT_EQ(parsed->relative_key, "foo/bar/");
}

TEST(StorageNodeQueryTest, RejectsNonTerminalSelector) {
  EXPECT_FALSE(ParseStorageQuery("sitos", "sitos/base/foo/**/bar").has_value());
  EXPECT_FALSE(ParseStorageQuery("sitos", "sitos/base/foo/*/bar").has_value());
}

TEST(StorageNodeQueryTest, PreservesPrefixChunkBoundary) {
  auto parsed = ParseStorageQuery("sitos", "sitos/base/foobar/**");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->is_list);
  EXPECT_EQ(parsed->relative_key, "foobar/");
  EXPECT_FALSE(ParseStorageQuery("sitos", "sitos/base/foo/**extra").has_value());
}

TEST(StorageNodeQueryTest, UsesDefaultPrefix) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node(transport);

  ASSERT_TRUE(node.Start(engine, {}).IsOk());
  EXPECT_EQ(transport.declared_keyexpr, "sitos/**");
}

TEST(StorageNodeQueryTest, RejectsInvalidStartArguments) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;

  StorageNode no_transport;
  EXPECT_FALSE(no_transport.Start(engine, {}).IsOk());

  StorageNode node(transport);
  EXPECT_FALSE(node.Start(nullptr, {}).IsOk());
  EXPECT_FALSE(node.Start(engine, {.prefix = ""}).IsOk());
  ASSERT_TRUE(node.Start(engine, {}).IsOk());
  EXPECT_FALSE(node.Start(engine, {}).IsOk());
}

TEST(StorageNodeQueryTest, RoutesExactGetAndPreservesPayload) {
  auto engine = std::make_shared<InMemoryEngine>();
  const std::vector<std::byte> payload = {std::byte{0x00}, std::byte{0xFF}};
  ASSERT_TRUE(engine->Put("foo/bar", payload));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  EXPECT_EQ(transport.declared_keyexpr, "sitos/**");

  auto replies = transport.Invoke("sitos/base/foo/bar");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].key, "sitos/base/foo/bar");
  EXPECT_EQ(replies[0].payload, payload);
  EXPECT_EQ(replies[0].encoding.id, Encoding::kSitosV1);
}

TEST(StorageNodeQueryTest, RoutesListAtChunkBoundary) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("foo/bar", std::vector<std::byte>{std::byte{0x01}}));
  ASSERT_TRUE(engine->Put("foobar/baz", std::vector<std::byte>{std::byte{0x02}}));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  auto replies = transport.Invoke("sitos/base/foo/**");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].key, "sitos/base/foo/bar");
}

TEST(StorageNodeQueryTest, UnknownExactGetProducesNoReply) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  EXPECT_TRUE(transport.Invoke("sitos/base/missing").empty());
}

TEST(StorageNodeQueryTest, ReplyFailureStopsList) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("foo/one", std::vector<std::byte>{std::byte{0x01}}));
  ASSERT_TRUE(engine->Put("foo/two", std::vector<std::byte>{std::byte{0x02}}));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  auto replies = transport.Invoke("sitos/base/foo/**", true);
  EXPECT_EQ(replies.size(), 1u);
}

TEST(StorageNodeQueryTest, StopDisablesExistingCallback) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("foo", std::vector<std::byte>{std::byte{0x01}}));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  node.Stop();

  EXPECT_TRUE(transport.Invoke("sitos/base/foo").empty());
  EXPECT_FALSE(node.IsStarted());
}

TEST(StorageNodeQueryTest, StopAndRestartReplacesDeclaration) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("foo", std::vector<std::byte>{std::byte{0x01}}));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  node.Stop();

  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  auto replies = transport.Invoke("sitos/base/foo");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].key, "sitos/base/foo");
}

}  // namespace
}  // namespace sitos
