// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode Session liveliness tokens (Issue #20, ADR-0037 §D2).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fence_internal.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"
#include "sitos/storage_node.hpp"
#include "storage_node_test_access.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace {

using sitos::Encoding;
using sitos::InMemoryEngine;
using sitos::LivelinessToken;
using sitos::PutOptions;
using sitos::Queryable;
using sitos::Result;
using sitos::Status;
using sitos::StorageNode;
using sitos::Subscription;
using sitos::Transport;
using sitos::TransportQuery;
using sitos::TransportSample;
using NodeAccess = sitos::storage_node_test_access::StorageNodeTestAccess;

class TokenFakeTransport final : public Transport {
 public:
  bool SupportsLiveliness() const noexcept override { return supports_liveliness; }

  Result<LivelinessToken> DeclareLivelinessToken(std::string_view key) override {
    const std::string owned(key);
    if (fail_token) return Result<LivelinessToken>::Err(Status::Error, "injected token failure");
    if (declare_hook) declare_hook(owned);
    {
      std::lock_guard lock(mutex);
      declared.push_back(owned);
      live.push_back(owned);
    }
    return Result<LivelinessToken>::Ok(LivelinessToken([this, owned] {
      if (withdraw_hook) withdraw_hook(owned);
      std::lock_guard lock(mutex);
      std::erase(live, owned);
    }));
  }

  Result<void> Put(std::string_view, std::span<const std::byte>, Encoding, PutOptions) override {
    return Result<void>::Ok();
  }
  Result<void> Delete(std::string_view, PutOptions) override { return Result<void>::Ok(); }
  Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    return Result<void>::Ok();
  }
  Result<Subscription> DeclareSubscriber(std::string_view,
                                         std::function<void(const TransportSample&)>) override {
    return Result<Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }
  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)> callback) override {
    query_callback = std::move(callback);
    return Result<Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

  std::size_t ReplyCount(std::string keyexpr) {
    std::size_t replies = 0;
    auto query = TransportQuery::ForTesting(
        [&replies](std::string_view, std::span<const std::byte>, Encoding) {
          ++replies;
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(keyexpr);
    query_callback(query);
    return replies;
  }

  std::vector<std::string> Live() {
    std::lock_guard lock(mutex);
    return live;
  }

  std::mutex mutex;
  bool supports_liveliness = true;
  bool fail_token = false;
  std::function<void(const std::string&)> declare_hook;
  std::function<void(const std::string&)> withdraw_hook;
  std::function<void(TransportQuery&)> query_callback;
  std::vector<std::string> declared;
  std::vector<std::string> live;
};

class RecordingSink final : public sitos::LogSink {
 public:
  void Write(const sitos::LogRecord& record) override {
    std::lock_guard lock(mutex);
    messages.emplace_back(record.message);
  }
  std::mutex mutex;
  std::vector<std::string> messages;
};

std::string ExpectedKey(StorageNode& node, std::string_view sid) {
  const auto generation = NodeAccess::SessionGeneration(node, sid);
  if (!generation.has_value()) return {};
  return "sitos/meta/live/session/" + std::string(sid) + "/" +
         sitos::fence_internal::FormatFenceUuid(*generation);
}

TEST(StorageNodeLivelinessTest, CreateSessionDeclaresTheGenerationTokenAfterTheSessionServesReads) {
  TokenFakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport, {.prefix = "sitos"}).IsOk());
  std::size_t meta_replies_at_declaration = 0;
  std::vector<std::string> active_at_declaration;
  transport.declare_hook = [&](const std::string&) {
    // Runs inside the declaration: the Session must already serve reads, and the node must not
    // hold a lock that a callback needs (ADR-0037 §D2).
    meta_replies_at_declaration = transport.ReplyCount("sitos/meta/session/s1");
    active_at_declaration = node.ActiveSessions();
  };

  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  ASSERT_EQ(transport.declared.size(), 1U);
  EXPECT_EQ(transport.declared[0], ExpectedKey(node, "s1"));
  EXPECT_EQ(transport.Live(), transport.declared);
  EXPECT_EQ(meta_replies_at_declaration, 1U);
  EXPECT_EQ(active_at_declaration, std::vector<std::string>{"s1"});
}

TEST(StorageNodeLivelinessTest, CloseSessionWithdrawsTheTokenAndARecreatedSessionGetsANewOne) {
  TokenFakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  std::vector<std::string> active_at_withdrawal{"unset"};
  transport.withdraw_hook = [&](const std::string&) { active_at_withdrawal = node.ActiveSessions(); };

  ASSERT_TRUE(node.CloseSession("s1").IsOk());

  EXPECT_TRUE(transport.Live().empty());
  EXPECT_TRUE(active_at_withdrawal.empty()) << "the token outlived the active Session";

  transport.withdraw_hook = {};
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  ASSERT_EQ(transport.declared.size(), 2U);
  EXPECT_NE(transport.declared[0], transport.declared[1]);
  EXPECT_EQ(transport.Live(), std::vector<std::string>{ExpectedKey(node, "s1")});
}

TEST(StorageNodeLivelinessTest, StopWithdrawsEveryToken) {
  TokenFakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  ASSERT_TRUE(node.CreateSession("s2").IsOk());
  ASSERT_EQ(transport.Live().size(), 2U);

  node.Stop();

  EXPECT_TRUE(transport.Live().empty());
}

TEST(StorageNodeLivelinessTest, ATransportWithoutLivelinessDeclaresNoToken) {
  TokenFakeTransport transport;
  transport.supports_liveliness = false;
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport, {.prefix = "sitos"}).IsOk());

  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  EXPECT_TRUE(transport.declared.empty());
}

TEST(StorageNodeLivelinessTest, AFailedDeclarationIsLoggedAndDoesNotFailCreateSession) {
  TokenFakeTransport transport;
  transport.fail_token = true;
  auto sink = std::make_shared<RecordingSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport,
                         {.prefix = "sitos", .log_sink = sink})
                  .IsOk());

  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  EXPECT_EQ(node.ActiveSessions(), std::vector<std::string>{"s1"});
  EXPECT_TRUE(transport.Live().empty());
  std::lock_guard lock(sink->mutex);
  EXPECT_TRUE(std::ranges::any_of(sink->messages, [](const std::string& message) {
    return message.find("liveliness") != std::string::npos;
  }));
}

TEST(StorageNodeLivelinessTest, ATokenDeclaredForASessionThatClosedMeanwhileIsWithdrawn) {
  TokenFakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), transport, {.prefix = "sitos"}).IsOk());
  // CloseSession wins the race against the declaration that CreateSession is still making.
  transport.declare_hook = [&](const std::string&) {
    transport.declare_hook = {};
    EXPECT_TRUE(node.CloseSession("s1").IsOk());
  };

  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  EXPECT_EQ(transport.declared.size(), 1U);
  EXPECT_TRUE(transport.Live().empty()) << "a token outlived the active state of its Session";
  EXPECT_TRUE(node.ActiveSessions().empty());
}

}  // namespace
