// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/storage_node.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"

namespace sitos {
namespace {

class LifetimeSink final : public LogSink {
 public:
  void Write(const LogRecord&) override {}
};

class CaptureSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex);
    records.push_back(record);
  }

  std::vector<LogRecord> Records() const {
    std::lock_guard<std::mutex> lock(mutex);
    return records;
  }

 private:
  mutable std::mutex mutex;
  std::vector<LogRecord> records;
};

class FailingEngine final : public StorageEngine {
 public:
  bool Put(std::string_view, Bytes) override { return false; }
  bool Delete(std::string_view) override { return false; }
  bool Get(std::string_view, const EntrySink&) const override { return false; }
  bool List(std::string_view, const EntrySink&) const override { return false; }
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

  Subscription DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const TransportSample&)> callback) override {
    subscriber_keyexpr = std::string(keyexpr);
    subscriber_callback = std::move(callback);
    return {};
  }

  void InvokeSubscriber(std::string key, TransportSample::Kind kind,
                        std::vector<std::byte> payload, Encoding encoding) {
    if (!subscriber_callback) return;
    TransportSample sample{std::move(key), payload, std::move(encoding), std::nullopt, kind};
    subscriber_callback(sample);
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
  std::string subscriber_keyexpr;
  std::function<void(const TransportSample&)> subscriber_callback;
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

TEST(StorageNodeQueryTest, DeclaresSubscriberAndRoutesBasePut) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  EXPECT_EQ(transport.subscriber_keyexpr, "sitos/**");

  const std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0xFE}};
  transport.InvokeSubscriber("sitos/base/foo/bar", TransportSample::Kind::Put, payload,
                             Encoding{std::string(Encoding::kSitosV1)});
  std::vector<std::byte> stored;
  ASSERT_TRUE(engine->Get("foo/bar", [&](std::string_view, Bytes value) {
    stored.assign(value.begin(), value.end());
    return true;
  }));
  EXPECT_EQ(stored, payload);
}

TEST(StorageNodeQueryTest, UnknownEncodingWrapsPayloadAsBytes) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  auto sink = std::make_shared<CaptureSink>();
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  const std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0xFF}};
  transport.InvokeSubscriber("sitos/base/raw", TransportSample::Kind::Put, payload,
                             Encoding{"application/octet-stream"});
  std::vector<std::byte> stored;
  ASSERT_TRUE(engine->Get("raw", [&](std::string_view, Bytes value) {
    stored.assign(value.begin(), value.end());
    return true;
  }));
  EXPECT_EQ(stored, (std::vector<std::byte>{std::byte{0x04}, std::byte{0x01}, std::byte{0x02},
                                             std::byte{0xFF}}));
  auto records = sink->Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::kWarning);
  EXPECT_EQ(records[0].component, "node");
  EXPECT_EQ(records[0].message, "unknown subscriber encoding; wrapped as bytes");
  auto replies = transport.Invoke("sitos/base/raw");
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].payload, stored);
  EXPECT_EQ(replies[0].encoding.id, Encoding::kSitosV1);
}

TEST(StorageNodeQueryTest, RoutesBaseDelete) {
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("foo", std::vector<std::byte>{std::byte{0x01}}));
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  transport.InvokeSubscriber("sitos/base/foo", TransportSample::Kind::Delete,
                             {std::byte{0xFF}}, Encoding{"unknown"});
  EXPECT_FALSE(engine->Get("foo", [](std::string_view, Bytes) { return true; }));
}

TEST(StorageNodeQueryTest, IgnoresUnsupportedSubscriberPathsWithWarnings) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  auto sink = std::make_shared<CaptureSink>();
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  const std::vector<std::byte> payload = {std::byte{0x07}};
  for (const auto* key : {"sitos/snap/s1/value", "sitos/session/s1/value",
                          "sitos/meta/session/s1", "sitos/base/$batch", "sitos/base/bad*"}) {
    transport.InvokeSubscriber(key, TransportSample::Kind::Put, payload,
                               Encoding{"application/octet-stream"});
  }
  EXPECT_FALSE(engine->Get("value", [](std::string_view, Bytes) { return true; }));
  auto records = sink->Records();
  ASSERT_EQ(records.size(), 5u);
  for (const auto& record : records) {
    EXPECT_EQ(record.level, LogLevel::kWarning);
    EXPECT_EQ(record.component, "node");
    EXPECT_EQ(record.message, "unsupported subscriber key");
  }
}

TEST(StorageNodeQueryTest, EngineFailureEmitsError) {
  auto engine = std::make_shared<FailingEngine>();
  FakeTransport transport;
  StorageNode node;
  auto sink = std::make_shared<CaptureSink>();
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());
  transport.InvokeSubscriber("sitos/base/value", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  transport.InvokeSubscriber("sitos/base/value", TransportSample::Kind::Delete, {}, {});
  auto records = sink->Records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].level, LogLevel::kError);
  EXPECT_EQ(records[0].message, "subscriber PUT failed");
  EXPECT_EQ(records[1].level, LogLevel::kError);
  EXPECT_EQ(records[1].message, "subscriber DELETE failed");
}

TEST(StorageNodeQueryTest, SubscriberCallbackBecomesInertAfterStop) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  auto callback = transport.subscriber_callback;
  node.Stop();
  ASSERT_TRUE(callback);
  std::vector<std::byte> payload = {std::byte{0x01}};
  TransportSample sample{"sitos/base/foo", payload,
                         Encoding{std::string(Encoding::kSitosV1)}, std::nullopt,
                         TransportSample::Kind::Put};
  callback(sample);
  EXPECT_FALSE(engine->Get("foo", [](std::string_view, Bytes) { return true; }));
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
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 1u);
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
  transport.InvokeSubscriber("sitos/snap/session-1/ignored", put, {std::byte{0x01}},
                             Encoding{"unknown"});
  transport.InvokeSubscriber("sitos/snap/session-1/ignored", del, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/session/session-1/ignored", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/meta/session/session-1", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/base/$batch", put, {std::byte{0x01}}, {});
  transport.InvokeSubscriber("sitos/base/", put, {std::byte{0x01}}, {});

  EXPECT_FALSE(engine->Get("ignored", [](std::string_view, Bytes) { return true; }));
  ASSERT_TRUE(engine->Get("existing", [](std::string_view, Bytes value) {
    EXPECT_EQ(std::vector<std::byte>(value.begin(), value.end()),
              (std::vector<std::byte>{std::byte{0x07}}));
    return true;
  }));
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 6u);
  for (const auto& record : records) {
    EXPECT_EQ(record.level, LogLevel::kWarning);
    EXPECT_EQ(record.component, "node");
    EXPECT_EQ(record.message, "unsupported subscriber key");
  }
}

TEST(StorageNodeSubscriberTest, RetainedCallbackIsInertAfterStop) {
  auto engine = std::make_shared<InMemoryEngine>();
  FakeTransport transport;
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos"}).IsOk());
  node.Stop();

  transport.InvokeSubscriber("sitos/base/after-stop", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  EXPECT_FALSE(engine->Get("after-stop", [](std::string_view, Bytes) { return true; }));
}

TEST(StorageNodeSubscriberTest, LogsEngineWriteFailure) {
  auto engine = std::make_shared<FailingEngine>();
  FakeTransport transport;
  auto sink = std::make_shared<CaptureSink>();
  StorageNode node;
  ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());

  transport.InvokeSubscriber("sitos/base/failure", TransportSample::Kind::Put,
                             {std::byte{0x01}}, Encoding{std::string(Encoding::kSitosV1)});
  const auto records = sink->Records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::kError);
  EXPECT_EQ(records[0].component, "node");
  EXPECT_EQ(records[0].message, "subscriber PUT failed");
}

}  // namespace
}  // namespace sitos
