// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/session_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
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

struct BlockingReadControl {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;

  void Enter() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [this] { return released; });
  }

  void WaitUntilEntered() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return entered; });
  }

  void Release() {
    {
      std::scoped_lock lock(mutex);
      released = true;
    }
    cv.notify_all();
  }
};

class BlockingSnapshotReader final : public StorageReader {
 public:
  BlockingSnapshotReader(std::shared_ptr<const StorageReader> inner,
                         std::shared_ptr<BlockingReadControl> control)
      : inner_(std::move(inner)), control_(std::move(control)) {}

  bool Get(std::string_view key, const EntrySink& sink) const override {
    control_->Enter();
    return inner_->Get(key, sink);
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    return inner_->List(prefix, sink);
  }

 private:
  std::shared_ptr<const StorageReader> inner_;
  std::shared_ptr<BlockingReadControl> control_;
};

class BlockingSnapshotEngine final : public InMemoryEngine {
 public:
  explicit BlockingSnapshotEngine(std::shared_ptr<BlockingReadControl> control)
      : control_(std::move(control)) {}

  std::shared_ptr<const StorageReader> TakeSnapshot() const override {
    return std::make_shared<BlockingSnapshotReader>(InMemoryEngine::TakeSnapshot(), control_);
  }

 private:
  std::shared_ptr<BlockingReadControl> control_;
};

class TrackingSnapshotReader final : public StorageReader {
 public:
  TrackingSnapshotReader(std::shared_ptr<const StorageReader> inner,
                         std::shared_ptr<int> token)
      : inner_(std::move(inner)), token_(std::move(token)) {}

  bool Get(std::string_view key, const EntrySink& sink) const override {
    return inner_->Get(key, sink);
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    return inner_->List(prefix, sink);
  }

 private:
  std::shared_ptr<const StorageReader> inner_;
  std::shared_ptr<int> token_;
};

class TrackingSnapshotEngine final : public InMemoryEngine {
 public:
  std::shared_ptr<const StorageReader> TakeSnapshot() const override {
    auto token = std::make_shared<int>(0);
    snapshot_token_ = token;
    return std::make_shared<TrackingSnapshotReader>(InMemoryEngine::TakeSnapshot(),
                                                    std::move(token));
  }

  std::weak_ptr<int> SnapshotToken() const { return snapshot_token_; }

 private:
  mutable std::weak_ptr<int> snapshot_token_;
};

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
  ASSERT_TRUE(engine->Put("hidden", std::vector<std::byte>{std::byte{0xff}}));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  const std::vector<std::byte> malformed{std::byte{0xff}};
  PublishValue("aaa-valid", ParamValue(std::int64_t{1}));
  PublishValue("hidden", ParamValue(std::int64_t{2}));
  transport.Publish("sitos/session/s1/zzz-malformed", malformed,
                    Encoding{std::string(Encoding::kSitosV1)});
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_EQ(view.Value().Get<std::int64_t>("hidden").Value(), 2);
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

TEST(SessionViewTest, ValidOverlayHidesMalformedSnapshotDuringList) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("shared", std::vector<std::byte>{std::byte{0xff}}));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  const auto overlay = ParamValue(std::int64_t{7}).Encode();
  transport.Publish("sitos/session/s1/shared", overlay, Encoding{std::string(Encoding::kSitosV1)});
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  std::vector<std::string> keys;
  EXPECT_TRUE(view.Value().List("", [&](std::string_view key, const ParamValue& value) {
    keys.emplace_back(key);
    EXPECT_EQ(value.As<std::int64_t>(), std::optional<std::int64_t>{7});
    return true;
  }).IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"shared"}));
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
  std::weak_ptr<InMemoryEngine> weak_engine = engine;
  auto view = [&] {
    auto node = std::make_unique<StorageNode>(transport);
    EXPECT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
    EXPECT_TRUE(node->CreateSession("s1").IsOk());
    return SessionView::Open(*node, "s1");
  }();
  ASSERT_TRUE(view.IsOk());
  engine.reset();
  EXPECT_TRUE(weak_engine.expired());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::Disconnected);
}

TEST(SessionViewTest, ListReleasesSnapshotBeforeSink) {
  TestTransport transport;
  auto engine = std::make_shared<TrackingSnapshotEngine>();
  ASSERT_TRUE(engine->Put("key", ParamValue(std::int64_t{1}).Encode()));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto snapshot_token = engine->SnapshotToken();
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  ASSERT_FALSE(snapshot_token.expired());
  auto listed = view.Value().List("", [&](std::string_view, const ParamValue&) {
    auto close = node.CloseSession("s1");
    EXPECT_TRUE(close.IsOk());
    EXPECT_TRUE(snapshot_token.expired());
    return true;
  });
  EXPECT_TRUE(listed.IsOk());
}

TEST(SessionViewTest, CapturedReadCompletesAcrossClose) {
  TestTransport transport;
  auto control = std::make_shared<BlockingReadControl>();
  auto engine = std::make_shared<BlockingSnapshotEngine>(control);
  ASSERT_TRUE(engine->Put("key", ParamValue(std::int64_t{1}).Encode()));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());

  Result<ParamValue> read = Result<ParamValue>::Err(Status::Error, "not started");
  std::thread reader([&] { read = view.Value().Get("key"); });
  control->WaitUntilEntered();
  ASSERT_TRUE(node.CloseSession("s1").IsOk());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::NotFound);
  control->Release();
  reader.join();
  ASSERT_TRUE(read.IsOk());
  auto value = read.Value().As<std::int64_t>();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 1);
}

TEST(SessionViewTest, StopWaitsForCapturedReadAndClosesAdmission) {
  TestTransport transport;
  auto control = std::make_shared<BlockingReadControl>();
  auto engine = std::make_shared<BlockingSnapshotEngine>(control);
  ASSERT_TRUE(engine->Put("key", ParamValue(std::int64_t{1}).Encode()));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  const auto quick_payload = ParamValue(std::int64_t{2}).Encode();
  transport.Publish("sitos/session/s1/quick", quick_payload,
                    Encoding{std::string(Encoding::kSitosV1)});

  std::thread reader([&] { EXPECT_TRUE(view.Value().Get("key").IsOk()); });
  control->WaitUntilEntered();
  std::atomic<bool> stop_started = false;
  std::atomic<bool> stop_returned = false;
  std::thread stopper([&] {
    stop_started = true;
    node.Stop();
    stop_returned = true;
  });
  while (!stop_started.load()) std::this_thread::yield();
  while (view.Value().Get("quick").StatusCode() != Status::Disconnected) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(stop_returned.load());
  control->Release();
  reader.join();
  stopper.join();
  EXPECT_TRUE(stop_returned.load());
}

TEST(SessionViewTest, ConcurrentReadsAndOverlayWritesAreSynchronized) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  std::barrier start(3);
  std::atomic<bool> failed = false;
  auto reader = [&] {
    start.arrive_and_wait();
    for (int i = 0; i < 200; ++i) {
      auto result = view.Value().Get("key");
      if (!result.IsOk() && result.StatusCode() != Status::NotFound) failed = true;
      auto listed = view.Value().List("", [](std::string_view, const ParamValue&) { return true; });
      if (!listed.IsOk()) failed = true;
    }
  };
  std::thread get_reader(reader);
  std::thread list_reader(reader);
  start.arrive_and_wait();
  for (int i = 0; i < 200; ++i) {
    auto payload = ParamValue(std::int64_t{i}).Encode();
    transport.Publish("sitos/session/s1/key", payload,
                      Encoding{std::string(Encoding::kSitosV1)});
  }
  get_reader.join();
  list_reader.join();
  EXPECT_FALSE(failed.load());
}

TEST(SessionViewTest, ListRejectsMalformedNonShadowedSnapshot) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  ASSERT_TRUE(engine->Put("aaa-valid", ParamValue(std::int64_t{1}).Encode()));
  ASSERT_TRUE(engine->Put("zzz-malformed", std::vector<std::byte>{std::byte{0xff}}));
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  int callbacks = 0;
  auto listed = view.Value().List("", [&](std::string_view, const ParamValue&) {
    ++callbacks;
    return true;
  });
  EXPECT_EQ(listed.StatusCode(), Status::Error);
  EXPECT_EQ(callbacks, 0);
}

TEST_F(SessionViewFixture, ListRawPrefixMatrixUsesOverlayPriorityAndLexicalOrder) {
  ASSERT_TRUE(engine->Put("foo", ParamValue(std::int64_t{1}).Encode()));
  ASSERT_TRUE(engine->Put("foo/bar", ParamValue(std::int64_t{2}).Encode()));
  ASSERT_TRUE(engine->Put("foo/bar/baz", ParamValue(std::int64_t{3}).Encode()));
  ASSERT_TRUE(engine->Put("foobar", ParamValue(std::int64_t{4}).Encode()));
  ASSERT_TRUE(engine->Put("unrelated", ParamValue(std::int64_t{5}).Encode()));
  node->Stop();
  ASSERT_TRUE(node->Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  PublishValue("foo", ParamValue(std::int64_t{10}));
  PublishValue("foo/bar", ParamValue(std::int64_t{20}));
  PublishValue("foo/qux", ParamValue(std::int64_t{30}));
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());

  const auto collect = [&](std::string_view prefix) {
    std::vector<std::pair<std::string, std::int64_t>> entries;
    auto listed = view.Value().List(prefix, [&](std::string_view key, const ParamValue& value) {
      const auto number = value.As<std::int64_t>();
      EXPECT_TRUE(number.has_value());
      entries.emplace_back(key, *number);
      return true;
    });
    EXPECT_TRUE(listed.IsOk());
    return entries;
  };

  EXPECT_EQ(collect(""), (std::vector<std::pair<std::string, std::int64_t>>{
                             {"foo", 10}, {"foo/bar", 20}, {"foo/bar/baz", 3},
                             {"foo/qux", 30}, {"foobar", 4}, {"unrelated", 5}}));
  EXPECT_EQ(collect("foo"), (std::vector<std::pair<std::string, std::int64_t>>{
                                {"foo", 10}, {"foo/bar", 20}, {"foo/bar/baz", 3},
                                {"foo/qux", 30}, {"foobar", 4}}));
  EXPECT_EQ(collect("foo/"), (std::vector<std::pair<std::string, std::int64_t>>{
                                 {"foo/bar", 20}, {"foo/bar/baz", 3}, {"foo/qux", 30}}));
  EXPECT_EQ(collect("foo/bar"), (std::vector<std::pair<std::string, std::int64_t>>{
                                    {"foo/bar", 20}, {"foo/bar/baz", 3}}));
  EXPECT_EQ(collect("foobar"),
            (std::vector<std::pair<std::string, std::int64_t>>{{"foobar", 4}}));

  int callbacks = 0;
  EXPECT_TRUE(view.Value().List("foo", [&](std::string_view key, const ParamValue&) {
    ++callbacks;
    EXPECT_EQ(key, "foo");
    return false;
  }).IsOk());
  EXPECT_EQ(callbacks, 1);
}

TEST_F(SessionViewFixture, ContainsPreservesSessionLifecycleNotFound) {
  auto view = SessionView::Open(*node, "s1");
  ASSERT_TRUE(view.IsOk());
  EXPECT_FALSE(view.Value().Contains("missing").Value());
  ASSERT_TRUE(node->CloseSession("s1").IsOk());
  EXPECT_EQ(view.Value().Contains("missing").StatusCode(), Status::NotFound);
  ASSERT_TRUE(node->CreateSession("s1").IsOk());
  EXPECT_EQ(view.Value().Contains("missing").StatusCode(), Status::NotFound);
}

TEST_F(SessionViewFixture, NullSinkAndMovedFromOperationsHaveInvalidArgumentPrecedence) {
  auto opened = SessionView::Open(*node, "s1");
  ASSERT_TRUE(opened.IsOk());
  SessionView source = std::move(opened).Value();
  SessionView moved(std::move(source));

  EXPECT_EQ(source.Get("bad/key").StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(source.Get<std::int64_t>("bad/key").StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(source.GetOr<std::int64_t>("bad/key", 1).StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(source.Contains("bad/key").StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(source.List("bad//prefix", {}).StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(moved.List("", {}).StatusCode(), Status::InvalidArgument);

  auto destination_result = SessionView::Open(*node, "s1");
  ASSERT_TRUE(destination_result.IsOk());
  SessionView destination = std::move(destination_result).Value();
  destination = std::move(moved);
  EXPECT_EQ(moved.Get("key").StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(destination.Get("missing").StatusCode(), Status::NotFound);
  AssignToSelf(destination);
  EXPECT_EQ(destination.Get("missing").StatusCode(), Status::NotFound);
}

TEST(SessionViewTest, OpenRacingStopHasOnlyAdmissibleOutcomes) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  std::barrier start(3);
  Result<SessionView> opened = Result<SessionView>::Err(Status::Error, "not started");
  std::thread opener([&] {
    start.arrive_and_wait();
    opened = SessionView::Open(node, "s1");
  });
  std::thread stopper([&] {
    start.arrive_and_wait();
    node.Stop();
  });
  start.arrive_and_wait();
  opener.join();
  stopper.join();
  EXPECT_TRUE(opened.IsOk() || opened.StatusCode() == Status::Disconnected);
  if (opened.IsOk()) {
    EXPECT_EQ(opened.Value().Get("key").StatusCode(), Status::Disconnected);
  }
}

TEST(SessionViewTest, ConcurrentReaderAndSessionReplacementHaveSafeOutcomes) {
  TestTransport transport;
  auto engine = std::make_shared<InMemoryEngine>();
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(engine, {.prefix = "sitos", .log_sink = nullptr}).IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  auto view = SessionView::Open(node, "s1");
  ASSERT_TRUE(view.IsOk());
  std::barrier start(2);
  std::atomic<bool> failed = false;
  std::thread reader([&] {
    start.arrive_and_wait();
    for (int i = 0; i < 200; ++i) {
      const auto get = view.Value().Get("key");
      if (!get.IsOk() && get.StatusCode() != Status::NotFound) failed = true;
      const auto list = view.Value().List("", [](std::string_view, const ParamValue&) {
        return true;
      });
      if (!list.IsOk() && list.StatusCode() != Status::NotFound) failed = true;
    }
  });
  start.arrive_and_wait();
  ASSERT_TRUE(node.CloseSession("s1").IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());
  reader.join();
  EXPECT_FALSE(failed.load());
  EXPECT_EQ(view.Value().Get("key").StatusCode(), Status::NotFound);
}

}  // namespace
}  // namespace sitos
