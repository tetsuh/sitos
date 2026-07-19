// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/session_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/param_value.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

static_assert(!std::is_copy_constructible_v<SessionView>);
static_assert(!std::is_copy_assignable_v<SessionView>);
static_assert(std::is_move_constructible_v<SessionView>);
static_assert(std::is_move_assignable_v<SessionView>);
template <typename T>
concept HasPut = requires(T& value) { value.Put("key", ParamValue(std::int64_t{1})); };
template <typename T>
concept HasPutBatch = requires(T& value) { value.PutBatch(std::span<const BatchEntry>{}); };
template <typename T>
concept HasDelete = requires(T& value) { value.Delete("key"); };
template <typename T>
concept HasGetShared = requires(const T& value) { value.GetShared("key"); };
template <typename T>
concept HasGetSpan = requires(const T& value) { value.template GetSpan<std::byte>("key"); };
static_assert(!HasPut<SessionView>);
static_assert(!HasPutBatch<SessionView>);
static_assert(!HasDelete<SessionView>);
static_assert(!HasGetShared<SessionView>);
static_assert(!HasGetSpan<SessionView>);

class TestTransport final : public Transport {
 public:
  Result<void> Put(std::string_view, std::span<const std::byte>, Encoding,
                   PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Result<void> Delete(std::string_view, PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    ++get_count;
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  Result<Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const TransportSample&)> callback) override {
    subscriber_keyexpr = std::string(keyexpr);
    subscriber = std::move(callback);
    return Result<Subscription>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeSubscription([this] {
          subscriber = {};
        }));
  }

  Result<Queryable> DeclareQueryable(
      std::string_view keyexpr,
      std::function<void(TransportQuery&)> callback) override {
    queryable_keyexpr = std::string(keyexpr);
    queryable = std::move(callback);
    return Result<Queryable>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeQueryable([this] {
          queryable = {};
        }));
  }

  void Publish(std::string key, std::span<const std::byte> payload, Encoding encoding,
               TransportSample::Kind kind = TransportSample::Kind::Put) {
    if (!subscriber) return;
    subscriber(TransportSample{std::move(key), payload, std::move(encoding), std::nullopt, kind});
  }

  int get_count = 0;
  std::string subscriber_keyexpr;
  std::string queryable_keyexpr;
  std::function<void(const TransportSample&)> subscriber;
  std::function<void(TransportQuery&)> queryable;
};

void AssignToSelf(SessionView& view) {
  auto* alias = &view;
  view = std::move(*alias);
}

class SessionViewFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    node = std::make_unique<StorageNode>(transport);
    ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
    ASSERT_TRUE(node->CreateSession("s1").IsOk());
  }

  void TearDown() override { node.reset(); }

  void PublishValue(std::string_view key, const ParamValue& value) {
    auto payload = value.Encode();
    transport.Publish("sitos/session/s1/" + std::string(key), payload,
                      Encoding{std::string(Encoding::kSitosV1)});
  }

  void PublishDelete(std::string_view key) {
    const std::vector<std::byte> empty;
    transport.Publish("sitos/session/s1/" + std::string(key), empty,
                      Encoding{std::string(Encoding::kSitosV1)}, TransportSample::Kind::Delete);
  }

  TestTransport transport;
  std::shared_ptr<InMemoryEngine> engine = std::make_shared<InMemoryEngine>();
  std::unique_ptr<StorageNode> node;
};

TEST_F(SessionViewFixture, ResolvesOverlayThenSnapshotAndContainsUsesResult) {
  ASSERT_TRUE(engine->Put("base", ParamValue(std::int64_t{7}).Encode()));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().Get<std::int64_t>("base").Value(), 7);
  PublishValue("base", ParamValue(std::int64_t{9}));
  EXPECT_EQ(view.Value().Get<std::int64_t>("base").Value(), 9);
  EXPECT_TRUE(view.Value().Contains("base").Value());
  EXPECT_FALSE(view.Value().Contains("missing").Value());
}

TEST_F(SessionViewFixture, ListMergesRawPrefixAndSorts) {
  ASSERT_TRUE(engine->Put("foo/base", ParamValue(std::int64_t{1}).Encode()));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  PublishValue("foo/base", ParamValue(std::int64_t{2}));
  PublishValue("foobar", ParamValue(std::int64_t{3}));
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  std::vector<std::string> keys;
  auto listed = view.Value().List("foo", [&](std::string_view key, const ParamValue&) {
    keys.emplace_back(key);
    return true;
  });
  ASSERT_TRUE(listed.IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/base", "foobar"}));
  EXPECT_EQ(transport.get_count, 0);
}

TEST_F(SessionViewFixture, MalformedOverlayDoesNotFallbackAndListDoesNotPartiallyCallback) {
  ASSERT_TRUE(engine->Put("bad", ParamValue(std::int64_t{1}).Encode()));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  const std::vector<std::byte> malformed{std::byte{0xff}};
  transport.Publish("sitos/session/s1/bad", malformed,
                    Encoding{std::string(Encoding::kSitosV1)});
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().Get("bad").StatusCode(), Status::Error);
  int callbacks = 0;
  EXPECT_EQ(view.Value().List("", [&](std::string_view, const ParamValue&) {
    ++callbacks;
    return true;
  }).StatusCode(), Status::Error);
  EXPECT_EQ(callbacks, 0);
}

TEST_F(SessionViewFixture, GetOrOnlySubstitutesNotFoundAndSinkCanStop) {
  PublishValue("value", ParamValue(std::int64_t{9}));
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().GetOr<std::int64_t>("missing", 42).Value(), 42);
  EXPECT_EQ(view.Value().GetOr<std::int64_t>("missing", 42).StatusCode(), Status::Ok);
  EXPECT_EQ(view.Value().GetOr<std::string>("value", "fallback").StatusCode(),
            Status::TypeMismatch);

  auto listed = view.Value().List("", [&](std::string_view, const ParamValue&) {
    node->Stop();
    return false;
  });
  EXPECT_TRUE(listed.IsOk());
}

TEST_F(SessionViewFixture, DeleteRestoresSnapshotAndListCallbacksAreOwnedAndReentrant) {
  ASSERT_TRUE(engine->Put("restore", ParamValue(std::int64_t{4}).Encode()));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  PublishValue("restore", ParamValue(std::int64_t{8}));
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  PublishDelete("restore");
  EXPECT_EQ(view.Value().Get<std::int64_t>("restore").Value(), 4);

  PublishValue("a", ParamValue(std::int64_t{1}));
  PublishValue("b", ParamValue(std::int64_t{2}));
  int callbacks = 0;
  auto listed = view.Value().List("", [&](std::string_view key, const ParamValue&) {
    ++callbacks;
    EXPECT_TRUE(view.Value().Contains(key).IsOk());
    return false;
  });
  EXPECT_TRUE(listed.IsOk());
  EXPECT_EQ(callbacks, 1);
}

TEST_F(SessionViewFixture, ListFalseAndExceptionHaveCallerThreadSemantics) {
  PublishValue("entry", ParamValue(std::int64_t{1}));
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  std::thread::id callback_thread;
  auto listed = view.Value().List("", [&](std::string_view, const ParamValue&) {
    callback_thread = std::this_thread::get_id();
    return false;
  });
  EXPECT_TRUE(listed.IsOk());
  EXPECT_EQ(callback_thread, std::this_thread::get_id());
  EXPECT_THROW(view.Value().List("", [&](std::string_view, const ParamValue&) -> bool {
    throw std::runtime_error("sink failure");
  }), std::runtime_error);
}

TEST(SessionViewTest, MalformedSnapshotIsErrorAndHiddenSnapshotIsNotDecoded) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  const std::vector<std::byte> malformed{std::byte{0xff}};
  ASSERT_TRUE(engine->Put("bad", malformed));
  ASSERT_TRUE(engine->Put("hidden", malformed));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().Get("bad").StatusCode(), Status::Error);
  transport.Publish("sitos/session/s1/hidden", ParamValue(std::int64_t{3}).Encode(),
                    Encoding{std::string(Encoding::kSitosV1)});
  EXPECT_EQ(view.Value().Get<std::int64_t>("hidden").Value(), 3);
}

TEST(SessionViewTest, MoveAndInvalidArgumentsAreDeterministic) {
  TestTransport transport;
  StorageNode node(transport);
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto source = SessionView::Open(node, "s1");
  ASSERT_TRUE(source.IsOk());
  SessionView moved(std::move(source).Value());
  EXPECT_EQ(source.Value().Get("key").StatusCode(), Status::InvalidArgument);
  SessionView assigned = std::move(moved);
  EXPECT_EQ(moved.Get("key").StatusCode(), Status::InvalidArgument);
  AssignToSelf(assigned);
  EXPECT_EQ(assigned.Get("key").StatusCode(), Status::NotFound);
}

TEST_F(SessionViewFixture, ClosedAndRecreatedSessionExpiresOldView) {
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  ASSERT_TRUE(node->CloseSession("s1").IsOk());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::NotFound);
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::NotFound);
}

TEST(SessionViewTest, OpenRejectsInvalidAndUnknownSession) {
  TestTransport transport;
  StorageNode node(transport);
  auto engine = std::make_shared<InMemoryEngine>();
  EXPECT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  EXPECT_EQ(SessionView::Open(node, "bad/id").StatusCode(), Status::InvalidKey);
  EXPECT_EQ(SessionView::Open(node, "missing").StatusCode(), Status::NotFound);
  node.Stop();
  EXPECT_EQ(SessionView::Open(node, "s1").StatusCode(), Status::Disconnected);
}

TEST(SessionViewTest, OutlivesNodeWithoutRetainingState) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  auto view = [&] {
    auto node = std::make_unique<StorageNode>(transport);
    EXPECT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
    EXPECT_TRUE(node->CreateSession("s1").IsOk());
    return SessionView::Open(*node, "s1");
  }();
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::Disconnected);
}

}  // namespace
}  // namespace sitos
