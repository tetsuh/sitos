// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/storage_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"
#include "sitos/param_value.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

static_assert(!std::is_copy_constructible_v<StorageNode>);
static_assert(!std::is_copy_assignable_v<StorageNode>);
static_assert(!std::is_move_constructible_v<StorageNode>);
static_assert(!std::is_move_assignable_v<StorageNode>);

class LifetimeSink final : public LogSink {
 public:
  void Write(const LogRecord&) override {}
};

struct CapturedLogRecord {
  LogLevel level;
  std::string component;
  std::string message;
};

class CaptureSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex);
    records.push_back(
        {.level = record.level,
         .component = std::string(record.component),
         .message = std::string(record.message)});
  }

  std::vector<CapturedLogRecord> Records() const {
    std::lock_guard<std::mutex> lock(mutex);
    return records;
  }

 private:
  mutable std::mutex mutex;
  std::vector<CapturedLogRecord> records;
};

class FailingEngine final : public StorageEngine {
 public:
  bool Put(std::string_view, Bytes) override { return false; }
  bool Delete(std::string_view) override { return false; }
  bool Get(std::string_view, const EntrySink&) const override { return false; }
  bool List(std::string_view, const EntrySink&) const override { return false; }
};

class BlockingEngine final : public StorageEngine {
 public:
  bool Put(std::string_view, Bytes) override {
    std::unique_lock<std::mutex> lock(mutex_);
    put_entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return release_; });
    return true;
  }

  bool Delete(std::string_view) override { return false; }

  bool Get(std::string_view, const EntrySink&) const override {
    std::unique_lock<std::mutex> lock(mutex_);
    get_entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return release_; });
    return true;
  }

  bool List(std::string_view, const EntrySink&) const override { return false; }

  void WaitForPut() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(cv_.wait_for(lock, std::chrono::seconds(2), [this] { return put_entered_; }));
  }

  void WaitForGet() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(cv_.wait_for(lock, std::chrono::seconds(2), [this] { return get_entered_; }));
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable bool get_entered_ = false;
  bool put_entered_ = false;
  bool release_ = false;
};

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

  Result<Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const TransportSample&)> callback) override {
    subscriber_keyexpr = std::string(keyexpr);
    subscriber_callback = std::move(callback);
    if (fail_subscriber) {
      subscriber_callback = {};
      return Result<Subscription>::Err(std::make_error_code(std::errc::io_error));
    }
    ++subscriber_declarations;
    return Result<Subscription>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeSubscription(
            [this] { ++subscriber_resets; }));
  }

  void InvokeSubscriber(std::string key, TransportSample::Kind kind,
                        std::vector<std::byte> payload, Encoding encoding) {
    if (!subscriber_callback) return;
    TransportSample sample{std::move(key), payload, std::move(encoding), std::nullopt, kind};
    subscriber_callback(sample);
  }

  Result<Queryable> DeclareQueryable(
      std::string_view keyexpr,
      std::function<void(TransportQuery&)> callback) override {
    declared_keyexpr = std::string(keyexpr);
    query_callback = std::move(callback);
    if (fail_queryable) {
      query_callback = {};
      return Result<Queryable>::Err(std::make_error_code(std::errc::io_error));
    }
    ++queryable_declarations;
    if (invoke_queryable_during_declare && query_callback) {
      auto query = TransportQuery::ForTesting(
          [](std::string_view, std::span<const std::byte>, Encoding) {
            return Result<void>::Ok();
          });
      query.keyexpr = std::string(keyexpr) + "/base/staged";
      query_callback(query);
    }
    return Result<Queryable>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeQueryable(
            [this] { ++queryable_resets; }));
  }

  std::vector<ReplyRecord> Invoke(std::string keyexpr, bool fail_after_first = false) {
    std::vector<ReplyRecord> replies;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, Encoding encoding) {
          replies.push_back(
              {std::string(key), std::vector<std::byte>(payload.begin(), payload.end()),
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
  std::string subscriber_keyexpr;
  std::function<void(const TransportSample&)> subscriber_callback;
  bool fail_queryable = false;
  bool fail_subscriber = false;
  bool invoke_queryable_during_declare = false;
  int queryable_declarations = 0;
  int subscriber_declarations = 0;
  int queryable_resets = 0;
  int subscriber_resets = 0;
};

TEST(DeclarationHandleTest, MoveConstructionTransfersResetHandlerExactlyOnce) {
  int subscription_resets = 0;
  {
    auto source = transport_test_access::DeclarationHandleTestAccess::MakeSubscription(
        [&] { ++subscription_resets; });
    {
      Subscription destination(std::move(source));
      EXPECT_EQ(subscription_resets, 0);
    }
    EXPECT_EQ(subscription_resets, 1);
  }
  EXPECT_EQ(subscription_resets, 1);

  int queryable_resets = 0;
  {
    auto source = transport_test_access::DeclarationHandleTestAccess::MakeQueryable(
        [&] { ++queryable_resets; });
    {
      Queryable destination(std::move(source));
      EXPECT_EQ(queryable_resets, 0);
    }
    EXPECT_EQ(queryable_resets, 1);
  }
  EXPECT_EQ(queryable_resets, 1);
}

TEST(DeclarationHandleTest, MoveAssignmentTransfersResetHandlerExactlyOnce) {
  int subscription_resets = 0;
  {
    Subscription destination;
    {
      auto source = transport_test_access::DeclarationHandleTestAccess::MakeSubscription(
          [&] { ++subscription_resets; });
      destination = std::move(source);
    }
    EXPECT_EQ(subscription_resets, 0);
  }
  EXPECT_EQ(subscription_resets, 1);

  int queryable_resets = 0;
  {
    Queryable destination;
    {
      auto source = transport_test_access::DeclarationHandleTestAccess::MakeQueryable(
          [&] { ++queryable_resets; });
      destination = std::move(source);
    }
    EXPECT_EQ(queryable_resets, 0);
  }
  EXPECT_EQ(queryable_resets, 1);
}

TEST(StorageNodeLifecycleTest, RollsBackPartialDeclarationAndAllowsRetry) {
  FakeTransport transport;
  transport.fail_subscriber = true;
  StorageNode node(transport);

  auto result = node.Start(std::make_shared<InMemoryEngine>(), {});
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.Error(), std::make_error_code(std::errc::io_error));
  EXPECT_FALSE(node.IsStarted());
  EXPECT_EQ(transport.queryable_resets, 1);
  EXPECT_EQ(transport.subscriber_declarations, 0);

  transport.fail_subscriber = false;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), {}).IsOk());
  EXPECT_TRUE(node.IsStarted());
  node.Stop();
  EXPECT_EQ(transport.queryable_resets, 2);
  EXPECT_EQ(transport.subscriber_resets, 1);
}

TEST(StorageNodeLifecycleTest, QueryableFailureDoesNotDeclareSubscriberAndCanRetry) {
  FakeTransport transport;
  transport.fail_queryable = true;
  StorageNode node(transport);

  auto result = node.Start(std::make_shared<InMemoryEngine>(), {});
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.Error(), std::make_error_code(std::errc::io_error));
  EXPECT_EQ(transport.subscriber_declarations, 0);
  EXPECT_FALSE(node.IsStarted());

  transport.fail_queryable = false;
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), {}).IsOk());
  node.Stop();
}

TEST(StorageNodeLifecycleTest, StagingCallbacksCannotTouchEngine) {
  FakeTransport transport;
  transport.invoke_queryable_during_declare = true;
  transport.fail_subscriber = true;
  auto engine = std::make_shared<InMemoryEngine>();
  StorageNode node(transport);

  EXPECT_FALSE(node.Start(engine, {}).IsOk());
  EXPECT_FALSE(engine->Get("staged", [](std::string_view, Bytes) { return true; }));
  EXPECT_FALSE(node.IsStarted());
}

TEST(StorageNodeLifecycleTest, StopWaitsForSubscriberCallback) {
  FakeTransport transport;
  auto engine = std::make_shared<BlockingEngine>();
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {}).IsOk());

  std::thread callback([&] {
    transport.InvokeSubscriber("sitos/base/key", TransportSample::Kind::Put,
                               {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  });
  engine->WaitForPut();

  std::atomic<bool> stopped{false};
  std::promise<void> stopper_called;
  auto stopper_ready = stopper_called.get_future();
  std::thread stopper([&] {
    stopper_called.set_value();
    node.Stop();
    stopped.store(true, std::memory_order_release);
  });
  stopper_ready.wait();
  EXPECT_FALSE(stopped.load(std::memory_order_acquire));
  engine->Release();
  callback.join();
  stopper.join();
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(node.IsStarted());
}

TEST(StorageNodeLifecycleTest, StopWaitsForQueryableCallback) {
  FakeTransport transport;
  auto engine = std::make_shared<BlockingEngine>();
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {}).IsOk());

  std::thread callback([&] { transport.Invoke("sitos/base/key"); });
  engine->WaitForGet();

  std::atomic<bool> stopped{false};
  std::promise<void> stopper_called;
  auto stopper_ready = stopper_called.get_future();
  std::thread stopper([&] {
    stopper_called.set_value();
    node.Stop();
    stopped.store(true, std::memory_order_release);
  });
  stopper_ready.wait();
  EXPECT_FALSE(stopped.load(std::memory_order_acquire));
  engine->Release();
  callback.join();
  stopper.join();
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(node.IsStarted());
}

TEST(StorageNodeLifecycleTest, ConcurrentStartsHaveOneWinner) {
  FakeTransport transport;
  StorageNode node(transport);
  std::barrier ready(3);
  std::array<std::optional<Result<void>>, 2> results;
  std::thread first([&] {
    ready.arrive_and_wait();
    results[0] = node.Start(std::make_shared<InMemoryEngine>(), {});
  });
  std::thread second([&] {
    ready.arrive_and_wait();
    results[1] = node.Start(std::make_shared<InMemoryEngine>(), {});
  });
  ready.arrive_and_wait();
  first.join();
  second.join();

  ASSERT_TRUE(results[0].has_value());
  ASSERT_TRUE(results[1].has_value());
  EXPECT_NE(results[0]->IsOk(), results[1]->IsOk());
  EXPECT_EQ(transport.queryable_declarations, 1);
  EXPECT_EQ(transport.subscriber_declarations, 1);
  node.Stop();
}

TEST(StorageNodeLifecycleTest, DestructionAfterStopDoesNotResetDeclarationsAgain) {
  FakeTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  {
    StorageNode node(transport);
    ASSERT_TRUE(node.Start(engine, {}).IsOk());
    node.Stop();
    EXPECT_EQ(transport.queryable_resets, 1);
    EXPECT_EQ(transport.subscriber_resets, 1);
  }

  EXPECT_EQ(transport.queryable_resets, 1);
  EXPECT_EQ(transport.subscriber_resets, 1);
  transport.InvokeSubscriber("sitos/base/after-destruction", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  EXPECT_FALSE(engine->Get("after-destruction", [](std::string_view, Bytes) { return true; }));
}

TEST(StorageNodeLifecycleTest, ConcurrentStopIsIdempotent) {
  FakeTransport transport;
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(std::make_shared<InMemoryEngine>(), {}).IsOk());
  std::barrier ready(3);
  std::thread first([&] {
    ready.arrive_and_wait();
    node.Stop();
  });
  std::thread second([&] {
    ready.arrive_and_wait();
    node.Stop();
  });
  ready.arrive_and_wait();
  first.join();
  second.join();
  EXPECT_FALSE(node.IsStarted());
  EXPECT_EQ(transport.queryable_resets, 1);
  EXPECT_EQ(transport.subscriber_resets, 1);
}

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

TEST(StorageNodeQueryTest, UsesDefaultLogSink) {
  StorageNodeConfig config;
  ASSERT_NE(config.log_sink, nullptr);
  EXPECT_EQ(config.log_sink, DefaultLogSink());

  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node(transport);

  ASSERT_TRUE(node.Start(engine, std::move(config)).IsOk());
  EXPECT_TRUE(node.IsStarted());
  node.Stop();
}

TEST(StorageNodeQueryTest, RetainsInjectedLogSinkWhileStarted) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node(transport);
  auto sink = std::make_shared<LifetimeSink>();
  const std::weak_ptr<LifetimeSink> weak_sink = sink;

  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = sink}).IsOk());
  sink.reset();
  EXPECT_FALSE(weak_sink.expired());

  node.Stop();
  EXPECT_FALSE(weak_sink.expired());

  // The test transport models undeclaration by releasing both callbacks.
  transport.query_callback = {};
  transport.subscriber_callback = {};
  EXPECT_TRUE(weak_sink.expired());
}

TEST(StorageNodeQueryTest, StartsWithLoggingDisabled) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node(transport);

  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  EXPECT_TRUE(node.IsStarted());
  node.Stop();
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

TEST(StorageNodeSubscriberTest, DeclaresPrefixAndRoutesKnownPutAndDelete) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  EXPECT_EQ(transport.subscriber_keyexpr, "sitos/**");

  const std::vector<std::byte> payload = {std::byte{0x04}, std::byte{0xAA}};
  transport.InvokeSubscriber("sitos/base/foo", TransportSample::Kind::Put, payload,
                             Encoding{std::string(Encoding::kSitosV1)});
  ASSERT_TRUE(engine->Get("foo", [&](std::string_view, Bytes value) {
    EXPECT_EQ(std::vector<std::byte>(value.begin(), value.end()), payload);
    return true;
  }));

  transport.InvokeSubscriber("sitos/base/foo", TransportSample::Kind::Delete, {std::byte{0xFF}},
                             Encoding{"unknown"});
  EXPECT_FALSE(engine->Get("foo", [](std::string_view, Bytes) { return true; }));
}

TEST(StorageNodeSubscriberTest, WrapsUnknownEncodingAsBytesPayloadV1) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  const std::vector<std::byte> raw = {std::byte{0x01}, std::byte{0x02}, std::byte{0xFF}};
  transport.InvokeSubscriber("sitos/base/opaque", TransportSample::Kind::Put, raw,
                             Encoding{"application/octet-stream"});
  const std::vector<std::byte> expected = {std::byte{0x04}, std::byte{0x01}, std::byte{0x02},
                                           std::byte{0xFF}};
  ASSERT_TRUE(engine->Get("opaque", [&](std::string_view, Bytes value) {
    EXPECT_EQ(std::vector<std::byte>(value.begin(), value.end()), expected);
    return true;
  }));
  const auto replies = transport.Invoke("sitos/base/opaque");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].payload, expected);
  EXPECT_EQ(replies[0].encoding.id, Encoding::kSitosV1);

  transport.InvokeSubscriber("sitos/base/empty", TransportSample::Kind::Put, {}, {});
  ASSERT_TRUE(engine->Get("empty", [&](std::string_view, Bytes value) {
    EXPECT_EQ(std::vector<std::byte>(value.begin(), value.end()),
              (std::vector<std::byte>{std::byte{0x04}}));
    return true;
  }));

  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].level, LogLevel::kWarning);
  EXPECT_EQ(records[0].component, "node");
  EXPECT_EQ(records[0].message, "unknown subscriber encoding; wrapped as bytes");
}

TEST(StorageNodeSubscriberTest, IgnoresUnsupportedPathsAndWarns) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("existing", std::vector<std::byte>{std::byte{0x07}}));
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  const auto put = TransportSample::Kind::Put;
  const auto del = TransportSample::Kind::Delete;
  // snap/** is read-only; session/<sid> with no active session is unknown; an
  // incorrectly encoded batch and remaining invalid paths are rejected.
  transport.InvokeSubscriber("sitos/snap/session-1/ignored", put, {std::byte{0x01}},
                             Encoding{"unknown"});
  transport.InvokeSubscriber("sitos/snap/session-1/ignored", del, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/session/session-1/ignored", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/meta/session/session-1", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/base/:batch", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/base/", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/base/bad*", put, {std::byte{0x01}}, {});

  EXPECT_FALSE(engine->Get("ignored", [](std::string_view, Bytes) { return true; }));
  ASSERT_TRUE(engine->Get("existing", [](std::string_view, Bytes value) {
    EXPECT_EQ(std::vector<std::byte>(value.begin(), value.end()),
              (std::vector<std::byte>{std::byte{0x07}}));
    return true;
  }));
  const std::vector<std::string> expected_messages = {
      "read-only snapshot key", "read-only snapshot key",  "unknown session",
      "unsupported subscriber key", "invalid batch operation or encoding",
      "unsupported subscriber key", "unsupported subscriber key"};
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), expected_messages.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(records[i].level, LogLevel::kWarning);
    EXPECT_EQ(records[i].component, "node");
    EXPECT_EQ(records[i].message, expected_messages[i]) << "record index " << i;
  }
}

TEST(StorageNodeSubscriberTest, RetainedCallbacksAreInertAfterStop) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  EXPECT_EQ(transport.queryable_declarations, 1);
  EXPECT_EQ(transport.subscriber_declarations, 1);
  node.Stop();
  EXPECT_EQ(transport.queryable_resets, 1);
  EXPECT_EQ(transport.subscriber_resets, 1);

  transport.InvokeSubscriber("sitos/base/after-stop", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  EXPECT_TRUE(transport.Invoke("sitos/base/after-stop").empty());
  EXPECT_FALSE(engine->Get("after-stop", [](std::string_view, Bytes) { return true; }));
}

TEST(StorageNodeSubscriberTest, LogsEngineWriteFailures) {
  auto engine = std::make_shared<FailingEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  transport.InvokeSubscriber("sitos/base/failure", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  transport.InvokeSubscriber("sitos/base/failure", TransportSample::Kind::Delete, {}, {});
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].level, LogLevel::kError);
  EXPECT_EQ(records[0].component, "node");
  EXPECT_EQ(records[0].message, "subscriber PUT failed");
  EXPECT_EQ(records[1].level, LogLevel::kError);
  EXPECT_EQ(records[1].component, "node");
  EXPECT_EQ(records[1].message, "subscriber DELETE failed");
}

// --- Session management (#12) -----------------------------------------------

namespace {

const Encoding kV1{std::string(Encoding::kSitosV1)};

// Puts a base value through the subscriber path, exactly as a remote client would.
void PutBase(FakeTransport& transport, std::string_view key, std::vector<std::byte> value) {
  transport.InvokeSubscriber(std::string("sitos/base/") + std::string(key),
                             TransportSample::Kind::Put, std::move(value), kV1);
}

void PutSession(FakeTransport& transport, std::string_view sid, std::string_view key,
                std::vector<std::byte> value) {
  transport.InvokeSubscriber(
      std::string("sitos/session/") + std::string(sid) + "/" + std::string(key),
      TransportSample::Kind::Put, std::move(value), kV1);
}

std::vector<std::byte> MakeBatch(
    std::initializer_list<std::pair<std::string, ParamValue>> entries) {
  return EncodeBatch(std::span<const std::pair<std::string, ParamValue>>(entries.begin(),
                                                                          entries.size()));
}

class FirstPutBlockingEngine final : public StorageEngine {
 public:
  bool Put(std::string_view key, Bytes value) override {
    std::unique_lock lock(mutex_);
    if (puts_.empty()) {
      first_put_entered_ = true;
      cv_.notify_all();
      cv_.wait(lock, [this] { return release_; });
    }
    puts_.push_back(std::string(key));
    values_[std::string(key)] = std::vector<std::byte>(value.begin(), value.end());
    return true;
  }

  bool Delete(std::string_view) override { return false; }
  bool Get(std::string_view key, const EntrySink& sink) const override {
    std::lock_guard lock(mutex_);
    auto it = values_.find(std::string(key));
    if (it == values_.end()) return false;
    sink(it->first, it->second);
    return true;
  }
  bool List(std::string_view, const EntrySink&) const override { return false; }

  void WaitForFirstPut() {
    std::unique_lock lock(mutex_);
    ASSERT_TRUE(cv_.wait_for(lock, std::chrono::seconds(2), [this] { return first_put_entered_; }));
  }
  void Release() {
    std::lock_guard lock(mutex_);
    release_ = true;
    cv_.notify_all();
  }
  std::vector<std::string> Puts() const {
    std::lock_guard lock(mutex_);
    return puts_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool first_put_entered_ = false;
  bool release_ = false;
  std::vector<std::string> puts_;
  std::map<std::string, std::vector<std::byte>, std::less<>> values_;
};

class FirstPutFailingEngine final : public StorageEngine {
 public:
  bool Put(std::string_view key, Bytes value) override {
    puts.push_back(std::string(key));
    if (puts.size() == 1) return false;
    return backing.Put(key, value);
  }
  bool Delete(std::string_view) override { return false; }
  bool Get(std::string_view key, const EntrySink& sink) const override {
    return backing.Get(key, sink);
  }
  bool List(std::string_view prefix, const EntrySink& sink) const override {
    return backing.List(prefix, sink);
  }

  std::vector<std::string> puts;

 private:
  InMemoryEngine backing;
};

}  // namespace

TEST(StorageNodeBatchTest, BaseBatchAppliesEntriesInOrderAndUsesPayloadV1) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  const auto batch = MakeBatch({{"a", ParamValue(std::int64_t{1})},
                                {"b", ParamValue("two")},
                                {"a", ParamValue(std::int64_t{3})}});
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  const auto a = transport.Invoke("sitos/base/a");
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].encoding.id, Encoding::kSitosV1);
  auto a_value = ParamValue::Decode(a[0].payload);
  ASSERT_TRUE(a_value.has_value());
  EXPECT_EQ(a_value->As<std::int64_t>(), 3);
  const auto b = transport.Invoke("sitos/base/b");
  ASSERT_EQ(b.size(), 1u);
  auto b_value = ParamValue::Decode(b[0].payload);
  ASSERT_TRUE(b_value.has_value());
  EXPECT_EQ(b_value->As<std::string>(), "two");
}

TEST(StorageNodeBatchTest, EmptyBatchIsValidNoOp) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  PutBase(transport, "existing", {std::byte{0x04}, std::byte{0x01}});
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, MakeBatch({}),
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  const auto existing = transport.Invoke("sitos/base/existing");
  ASSERT_EQ(existing.size(), 1u);
  EXPECT_EQ(existing[0].payload, (std::vector<std::byte>{std::byte{0x04}, std::byte{0x01}}));
  EXPECT_TRUE(sink->Records().empty());
}

TEST(StorageNodeBatchTest, SessionBatchOnlyUpdatesSelectedOverlay) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("one").IsOk());
  ASSERT_TRUE(node.CreateSession("two").IsOk());

  const auto batch = MakeBatch({{"setting", ParamValue("value")}});
  transport.InvokeSubscriber("sitos/session/one/:batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  ASSERT_EQ(transport.Invoke("sitos/session/one/setting").size(), 1u);
  EXPECT_TRUE(transport.Invoke("sitos/session/two/setting").empty());
  EXPECT_TRUE(transport.Invoke("sitos/base/setting").empty());
}

TEST(StorageNodeBatchTest, RejectsInvalidBatchBeforeAnyMutationAndWarns) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  auto truncated = MakeBatch({{"first", ParamValue(std::int64_t{1})},
                              {"truncated", ParamValue(std::int64_t{2})}});
  truncated.pop_back();
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, truncated,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  const auto invalid_key = MakeBatch({{"good", ParamValue(std::int64_t{1})},
                                      {"bad*", ParamValue(std::int64_t{2})}});
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, invalid_key,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  EXPECT_TRUE(transport.Invoke("sitos/base/first").empty());
  EXPECT_TRUE(transport.Invoke("sitos/base/good").empty());
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].level, LogLevel::kWarning);
  EXPECT_EQ(records[1].level, LogLevel::kWarning);
}

TEST(StorageNodeBatchTest, RejectsTrailingBytesAndUnknownTagWithoutMutation) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  auto trailing = MakeBatch({{"trailing", ParamValue(std::int64_t{1})}});
  trailing.push_back(std::byte{0x00});
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, trailing,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  // One entry: count, key length, key, invalid tag, and zero value length.
  const std::vector<std::byte> unknown_tag = {
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{'x'},  std::byte{0xFF}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}};
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, unknown_tag,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  EXPECT_TRUE(transport.Invoke("sitos/base/trailing").empty());
  EXPECT_TRUE(transport.Invoke("sitos/base/x").empty());
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].message, "malformed batch payload");
  EXPECT_EQ(records[1].message, "malformed batch payload");
}

TEST(StorageNodeBatchTest, RejectsWrongOperationAndEncodingButPreservesOrdinaryFallback) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());
  const auto batch = MakeBatch({{"entry", ParamValue(std::int64_t{1})}});

  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, batch, kV1);
  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Delete, {},
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  transport.InvokeSubscriber("sitos/base/$batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  transport.InvokeSubscriber("sitos/base/@batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  transport.InvokeSubscriber("sitos/base/~batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  transport.InvokeSubscriber("sitos/base/ordinary", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  EXPECT_TRUE(transport.Invoke("sitos/base/entry").empty());
  transport.InvokeSubscriber("sitos/session/nope/:batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  transport.InvokeSubscriber("sitos/snap/nope/:batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});
  EXPECT_TRUE(transport.Invoke("sitos/session/nope/entry").empty());
  const auto ordinary = transport.Invoke("sitos/base/ordinary");
  ASSERT_EQ(ordinary.size(), 1u);
  ASSERT_FALSE(ordinary[0].payload.empty());
  EXPECT_EQ(ordinary[0].payload[0], std::byte{0x04});
  EXPECT_EQ(sink->Records().size(), 8u);
}

TEST(StorageNodeBatchTest, ContinuesAfterEngineFailureInEncodedOrder) {
  auto engine = std::make_shared<FirstPutFailingEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());
  const auto batch = MakeBatch({{"first", ParamValue(std::int64_t{1})},
                                {"later", ParamValue(std::int64_t{2})}});

  transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, batch,
                             Encoding{std::string(Encoding::kSitosV1Batch)});

  EXPECT_EQ(engine->puts, (std::vector<std::string>{"first", "later"}));
  EXPECT_TRUE(transport.Invoke("sitos/base/first").empty());
  ASSERT_EQ(transport.Invoke("sitos/base/later").size(), 1u);
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::kError);
}

TEST(StorageNodeBatchTest, SerializesBatchAndOrdinaryWrites) {
  auto engine = std::make_shared<FirstPutBlockingEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  const auto batch = MakeBatch({{"first", ParamValue(std::int64_t{1})},
                                {"second", ParamValue(std::int64_t{2})}});

  std::thread batch_callback([&] {
    transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, batch,
                               Encoding{std::string(Encoding::kSitosV1Batch)});
  });
  engine->WaitForFirstPut();
  std::promise<void> ordinary_started;
  auto ordinary_ready = ordinary_started.get_future();
  std::thread ordinary_callback([&] {
    ordinary_started.set_value();
    PutBase(transport, "ordinary", {std::byte{0x04}, std::byte{0x01}});
  });
  ordinary_ready.wait();
  EXPECT_TRUE(engine->Puts().empty());

  engine->Release();
  batch_callback.join();
  ordinary_callback.join();
  EXPECT_EQ(engine->Puts(), (std::vector<std::string>{"first", "second", "ordinary"}));
}

TEST(StorageNodeBatchTest, StopWaitsForInFlightBatch) {
  auto engine = std::make_shared<FirstPutBlockingEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  const auto batch = MakeBatch({{"first", ParamValue(std::int64_t{1})}});

  std::thread batch_callback([&] {
    transport.InvokeSubscriber("sitos/base/:batch", TransportSample::Kind::Put, batch,
                               Encoding{std::string(Encoding::kSitosV1Batch)});
  });
  engine->WaitForFirstPut();
  std::atomic<bool> stopped{false};
  std::promise<void> stopper_called;
  auto stopper_ready = stopper_called.get_future();
  std::thread stopper([&] {
    stopper_called.set_value();
    node.Stop();
    stopped.store(true, std::memory_order_release);
  });
  stopper_ready.wait();
  EXPECT_FALSE(stopped.load(std::memory_order_acquire));

  engine->Release();
  batch_callback.join();
  stopper.join();
  EXPECT_TRUE(stopped.load(std::memory_order_acquire));
}

TEST(StorageNodeSessionTest, SnapshotIsIsolatedFromBasePut) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  const std::vector<std::byte> before = {std::byte{0x01}};
  const std::vector<std::byte> after = {std::byte{0x02}};
  PutBase(transport, "k", before);
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  PutBase(transport, "k", after);

  auto snap = transport.Invoke("sitos/snap/s1/k");
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].key, "sitos/snap/s1/k");
  EXPECT_EQ(snap[0].payload, before);

  auto base = transport.Invoke("sitos/base/k");
  ASSERT_EQ(base.size(), 1u);
  EXPECT_EQ(base[0].payload, after);
}

TEST(StorageNodeSessionTest, SnapshotListReflectsSnapshotOnly) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  PutBase(transport, "a/x", {std::byte{0x01}});
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  PutBase(transport, "a/y", {std::byte{0x02}});  // after snapshot

  auto replies = transport.Invoke("sitos/snap/s1/a/**");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].key, "sitos/snap/s1/a/x");
}

TEST(StorageNodeSessionTest, OverlayRoutesSessionPutAndGet) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  const std::vector<std::byte> value = {std::byte{0xAB}};
  PutSession(transport, "s1", "p", value);

  auto session = transport.Invoke("sitos/session/s1/p");
  ASSERT_EQ(session.size(), 1u);
  EXPECT_EQ(session[0].key, "sitos/session/s1/p");
  EXPECT_EQ(session[0].payload, value);

  // Overlay writes are isolated from base and from the snapshot.
  EXPECT_TRUE(transport.Invoke("sitos/base/p").empty());
  EXPECT_TRUE(transport.Invoke("sitos/snap/s1/p").empty());
}

TEST(StorageNodeSessionTest, OverlayDeleteRemovesValue) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  PutSession(transport, "s1", "p", {std::byte{0x01}});
  ASSERT_EQ(transport.Invoke("sitos/session/s1/p").size(), 1u);
  transport.InvokeSubscriber("sitos/session/s1/p", TransportSample::Kind::Delete, {}, {});
  EXPECT_TRUE(transport.Invoke("sitos/session/s1/p").empty());
}

TEST(StorageNodeSessionTest, OverlayListAtChunkBoundary) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  PutSession(transport, "s1", "a/x", {std::byte{0x01}});
  PutSession(transport, "s1", "a/y", {std::byte{0x02}});
  PutSession(transport, "s1", "b/z", {std::byte{0x03}});

  auto replies = transport.Invoke("sitos/session/s1/a/**");
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies[0].key, "sitos/session/s1/a/x");
  EXPECT_EQ(replies[1].key, "sitos/session/s1/a/y");
}

TEST(StorageNodeSessionTest, SnapshotWritesAreIgnoredReadOnly) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());
  PutBase(transport, "k", {std::byte{0x01}});
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  transport.InvokeSubscriber("sitos/snap/s1/k", TransportSample::Kind::Put, {std::byte{0x09}},
                             kV1);
  transport.InvokeSubscriber("sitos/snap/s1/k", TransportSample::Kind::Delete, {}, {});

  // The snapshot still returns its original value.
  auto snap = transport.Invoke("sitos/snap/s1/k");
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].payload, (std::vector<std::byte>{std::byte{0x01}}));

  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  for (const auto& record : records) {
    EXPECT_EQ(record.level, LogLevel::kWarning);
    EXPECT_EQ(record.message, "read-only snapshot key");
  }
}

TEST(StorageNodeSessionTest, UnknownSessionQueriesReplyZero) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  EXPECT_TRUE(transport.Invoke("sitos/snap/nope/k").empty());
  EXPECT_TRUE(transport.Invoke("sitos/session/nope/k").empty());
  EXPECT_TRUE(transport.Invoke("sitos/snap/nope/**").empty());
  EXPECT_TRUE(transport.Invoke("sitos/session/nope/**").empty());
}

TEST(StorageNodeSessionTest, WriteToUnknownSessionWarnsAndIgnores) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  PutSession(transport, "nope", "k", {std::byte{0x01}});
  EXPECT_TRUE(transport.Invoke("sitos/session/nope/k").empty());
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::kWarning);
  EXPECT_EQ(records[0].message, "unknown session");
}

TEST(StorageNodeSessionTest, MetaSessionReflectsLifecycle) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  EXPECT_TRUE(transport.Invoke("sitos/meta/session/s1").empty());

  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto meta = transport.Invoke("sitos/meta/session/s1");
  ASSERT_EQ(meta.size(), 1u);
  EXPECT_EQ(meta[0].key, "sitos/meta/session/s1");
  EXPECT_EQ(meta[0].encoding.id, Encoding::kSitosV1);
  auto decoded = ParamValue::Decode(meta[0].payload);
  ASSERT_TRUE(decoded.has_value());
  auto json = decoded->As<std::string>();
  ASSERT_TRUE(json.has_value());
  EXPECT_NE(json->find("\"state\":\"active\""), std::string::npos) << *json;
  EXPECT_NE(json->find("\"created_at\""), std::string::npos) << *json;

  ASSERT_TRUE(node.CloseSession("s1").IsOk());
  EXPECT_TRUE(transport.Invoke("sitos/meta/session/s1").empty());
}

TEST(StorageNodeSessionTest, CloseSessionReleasesSnapshotAndOverlay) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  PutBase(transport, "k", {std::byte{0x01}});
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  PutSession(transport, "s1", "p", {std::byte{0x02}});
  ASSERT_EQ(transport.Invoke("sitos/session/s1/p").size(), 1u);
  ASSERT_EQ(transport.Invoke("sitos/snap/s1/k").size(), 1u);

  ASSERT_TRUE(node.CloseSession("s1").IsOk());
  EXPECT_TRUE(transport.Invoke("sitos/snap/s1/k").empty());
  EXPECT_TRUE(transport.Invoke("sitos/session/s1/p").empty());
  EXPECT_TRUE(node.ActiveSessions().empty());
}

TEST(StorageNodeSessionTest, ValidatesSidAndRejectsDuplicate) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  EXPECT_EQ(node.CreateSession("").Error(), std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(node.CreateSession("bad/sid").Error(),
            std::make_error_code(std::errc::invalid_argument));

  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  EXPECT_EQ(node.CreateSession("s1").Error(), std::make_error_code(std::errc::file_exists));

  auto active = node.ActiveSessions();
  ASSERT_EQ(active.size(), 1u);
  EXPECT_EQ(active[0], "s1");
}

TEST(StorageNodeSessionTest, CloseUnknownSessionFails) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  EXPECT_EQ(node.CloseSession("nope").Error(),
            std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(StorageNodeSessionTest, SessionOpsRequireStartedNode) {
  FakeTransport transport;
  StorageNode node(transport);

  EXPECT_EQ(node.CreateSession("s1").Error(),
            std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(node.CloseSession("s1").Error(),
            std::make_error_code(std::errc::invalid_argument));
  EXPECT_TRUE(node.ActiveSessions().empty());
}

TEST(StorageNodeSessionTest, SessionsClearedAfterStop) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  node.Stop();
  EXPECT_TRUE(node.ActiveSessions().empty());
}

namespace {

// Blocks inside TakeSnapshot until released, so a CreateSession holding the
// callback-gate lease can be observed mid-flight.
class BlockingSnapshotEngine final : public InMemoryEngine {
 public:
  std::shared_ptr<const StorageReader> TakeSnapshot() const override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return release_; });
    return InMemoryEngine::TakeSnapshot();
  }

  void WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(cv_.wait_for(lock, std::chrono::seconds(2), [this] { return entered_; }));
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable bool entered_ = false;
  bool release_ = false;
};

}  // namespace

// Stop() must wait for an in-flight session operation: the lease keeps the node
// enrolled so teardown does not race a running CreateSession.
TEST(StorageNodeSessionTest, StopWaitsForInFlightCreateSession) {
  auto engine = std::make_shared<BlockingSnapshotEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  std::thread creator([&] { (void)node.CreateSession("s1"); });
  engine->WaitEntered();  // CreateSession is now inside TakeSnapshot holding the lease.

  std::atomic<bool> stopped{false};
  std::promise<void> stopper_called;
  auto stopper_ready = stopper_called.get_future();
  std::thread stopper([&] {
    stopper_called.set_value();
    node.Stop();
    stopped.store(true, std::memory_order_release);
  });
  stopper_ready.wait();
  EXPECT_FALSE(stopped.load(std::memory_order_acquire));  // Stop blocked on the lease.

  engine->Release();
  creator.join();
  stopper.join();
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(node.IsStarted());
}

// AC3: no accumulation or leaks across repeated create/close cycles. This runs
// under the sanitizer CI job (ASan/TSan) via the StorageNodeSessionTest filter.
TEST(StorageNodeSessionTest, CreateCloseLoopReleasesResources) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(node.CreateSession("s1").IsOk());
    PutSession(transport, "s1", "p", {std::byte{0x01}});
    ASSERT_EQ(transport.Invoke("sitos/session/s1/p").size(), 1u);
    ASSERT_TRUE(node.CloseSession("s1").IsOk());
    EXPECT_TRUE(transport.Invoke("sitos/session/s1/p").empty());
  }
  EXPECT_TRUE(node.ActiveSessions().empty());
}

}  // namespace
}  // namespace sitos
