// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the Transport abstraction using a real zenoh session.

#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include "src/transport/zenoh_transport_test_access.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

TEST(TransportApiTest, TransportQueryCannotOutliveCallback) {
  static_assert(!std::is_move_constructible_v<sitos::TransportQuery>);
  static_assert(!std::is_move_assignable_v<sitos::TransportQuery>);
}

struct CallbackCounter {
  std::mutex mutex;
  std::condition_variable condition;
  int count = 0;
};

void CountCallback(CallbackCounter* state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->count;
  }
  state->condition.notify_all();
}

bool WaitForCount(CallbackCounter* state, int expected) {
  std::unique_lock<std::mutex> lock(state->mutex);
  return state->condition.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return state->count >= expected; });
}

int ReadCount(CallbackCounter* state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->count;
}

bool WaitForNoNewCallback(CallbackCounter* state, int baseline) {
  std::unique_lock<std::mutex> lock(state->mutex);
  return !state->condition.wait_for(lock, std::chrono::seconds(1),
                                    [&] { return state->count > baseline; });
}

class SubscriptionTest : public ::testing::Test {
 protected:
  static void TearDownTestSuite() {
    sitos::transport_test_access::SubscriptionTestAccess::Shutdown();
  }
};

void SelfMove(sitos::Subscription& subscription) {
  auto* alias = &subscription;
  subscription = std::move(*alias);
}

TEST_F(SubscriptionTest, MoveAssignmentReleasesOldSubscriber) {
  using sitos::transport_test_access::SubscriptionTestAccess;
  ASSERT_TRUE(SubscriptionTestAccess::IsAvailable());

  CallbackCounter callbacks;
  CallbackCounter observer_callbacks;
  auto observer = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-empty", [&] { CountCallback(&observer_callbacks); });
  auto subscription = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-empty", [&] { CountCallback(&callbacks); });
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-empty"));
  ASSERT_TRUE(WaitForCount(&callbacks, 1));
  ASSERT_TRUE(WaitForCount(&observer_callbacks, 1));

  sitos::Subscription empty;
  subscription = std::move(empty);
  const int baseline = ReadCount(&callbacks);
  const int observer_baseline = ReadCount(&observer_callbacks);
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-empty"));
  ASSERT_TRUE(WaitForCount(&observer_callbacks, observer_baseline + 1));
  EXPECT_TRUE(WaitForNoNewCallback(&callbacks, baseline));
}

TEST_F(SubscriptionTest, MoveAssignmentTransfersSubscriber) {
  using sitos::transport_test_access::SubscriptionTestAccess;
  ASSERT_TRUE(SubscriptionTestAccess::IsAvailable());

  CallbackCounter old_callbacks;
  CallbackCounter new_callbacks;
  CallbackCounter old_observer_callbacks;
  CallbackCounter new_observer_callbacks;
  auto old_observer = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-live-old",
      [&] { CountCallback(&old_observer_callbacks); });
  auto new_observer = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-live-new",
      [&] { CountCallback(&new_observer_callbacks); });
  auto target = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-live-old", [&] { CountCallback(&old_callbacks); });
  auto source = SubscriptionTestAccess::Make(
      "sitos/test/subscription/move-live-new", [&] { CountCallback(&new_callbacks); });
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-live-old"));
  ASSERT_TRUE(WaitForCount(&old_callbacks, 1));
  ASSERT_TRUE(WaitForCount(&old_observer_callbacks, 1));
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-live-new"));
  ASSERT_TRUE(WaitForCount(&new_callbacks, 1));
  ASSERT_TRUE(WaitForCount(&new_observer_callbacks, 1));

  target = std::move(source);
  const int old_baseline = ReadCount(&old_callbacks);
  const int new_baseline = ReadCount(&new_callbacks);
  const int old_observer_baseline = ReadCount(&old_observer_callbacks);
  const int new_observer_baseline = ReadCount(&new_observer_callbacks);
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-live-old"));
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/move-live-new"));
  ASSERT_TRUE(WaitForCount(&old_observer_callbacks, old_observer_baseline + 1));
  ASSERT_TRUE(WaitForCount(&new_observer_callbacks, new_observer_baseline + 1));
  ASSERT_TRUE(WaitForCount(&new_callbacks, new_baseline + 1));
  EXPECT_TRUE(WaitForNoNewCallback(&old_callbacks, old_baseline));
}

TEST_F(SubscriptionTest, DestructionStopsSubscriber) {
  using sitos::transport_test_access::SubscriptionTestAccess;
  ASSERT_TRUE(SubscriptionTestAccess::IsAvailable());

  CallbackCounter callbacks;
  CallbackCounter observer_callbacks;
  auto observer = SubscriptionTestAccess::Make(
      "sitos/test/subscription/destruction", [&] { CountCallback(&observer_callbacks); });
  {
    auto subscription = SubscriptionTestAccess::Make(
        "sitos/test/subscription/destruction", [&] { CountCallback(&callbacks); });
    ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/destruction"));
    ASSERT_TRUE(WaitForCount(&callbacks, 1));
    ASSERT_TRUE(WaitForCount(&observer_callbacks, 1));
  }

  const int baseline = ReadCount(&callbacks);
  const int observer_baseline = ReadCount(&observer_callbacks);
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/destruction"));
  ASSERT_TRUE(WaitForCount(&observer_callbacks, observer_baseline + 1));
  EXPECT_TRUE(WaitForNoNewCallback(&callbacks, baseline));
}

TEST_F(SubscriptionTest, TransferredSubscriberStopsAfterFinalDestruction) {
  using sitos::transport_test_access::SubscriptionTestAccess;
  ASSERT_TRUE(SubscriptionTestAccess::IsAvailable());

  CallbackCounter callbacks;
  CallbackCounter observer_callbacks;
  auto observer = SubscriptionTestAccess::Make(
      "sitos/test/subscription/transferred-destruction",
      [&] { CountCallback(&observer_callbacks); });
  {
    auto target = SubscriptionTestAccess::Make(
        "sitos/test/subscription/transferred-old", [] {});
    auto source = SubscriptionTestAccess::Make(
        "sitos/test/subscription/transferred-destruction",
        [&] { CountCallback(&callbacks); });
    ASSERT_TRUE(SubscriptionTestAccess::Publish(
        "sitos/test/subscription/transferred-destruction"));
    ASSERT_TRUE(WaitForCount(&callbacks, 1));
    ASSERT_TRUE(WaitForCount(&observer_callbacks, 1));

    target = std::move(source);
    const int active_baseline = ReadCount(&callbacks);
    const int active_observer_baseline = ReadCount(&observer_callbacks);
    ASSERT_TRUE(SubscriptionTestAccess::Publish(
        "sitos/test/subscription/transferred-destruction"));
    ASSERT_TRUE(WaitForCount(&observer_callbacks, active_observer_baseline + 1));
    ASSERT_TRUE(WaitForCount(&callbacks, active_baseline + 1));
  }

  const int final_baseline = ReadCount(&callbacks);
  const int final_observer_baseline = ReadCount(&observer_callbacks);
  ASSERT_TRUE(SubscriptionTestAccess::Publish(
      "sitos/test/subscription/transferred-destruction"));
  ASSERT_TRUE(WaitForCount(&observer_callbacks, final_observer_baseline + 1));
  EXPECT_TRUE(WaitForNoNewCallback(&callbacks, final_baseline));
}

TEST_F(SubscriptionTest, SelfMovePreservesSubscriber) {
  using sitos::transport_test_access::SubscriptionTestAccess;
  ASSERT_TRUE(SubscriptionTestAccess::IsAvailable());

  CallbackCounter callbacks;
  auto subscription = SubscriptionTestAccess::Make(
      "sitos/test/subscription/self-move", [&] { CountCallback(&callbacks); });
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/self-move"));
  ASSERT_TRUE(WaitForCount(&callbacks, 1));

  SelfMove(subscription);
  const int baseline = ReadCount(&callbacks);
  ASSERT_TRUE(SubscriptionTestAccess::Publish("sitos/test/subscription/self-move"));
  EXPECT_TRUE(WaitForCount(&callbacks, baseline + 1));
}

TEST_F(SubscriptionTest, EmptyMovesAreSafe) {
  sitos::Subscription empty;
  SelfMove(empty);
  sitos::Subscription moved_from = std::move(empty);
  moved_from = std::move(empty);
  sitos::Subscription assigned;
  assigned = std::move(moved_from);
}

TEST(ZenohEncodingTest, EmitsCanonicalSitosWireEncodings) {
  using sitos::transport_test_access::BuildWireEncoding;

  EXPECT_EQ(BuildWireEncoding({std::string(sitos::Encoding::kSitosV1)}),
            "zenoh/bytes;sitos.v1");
  EXPECT_EQ(BuildWireEncoding({"zenoh.bytes;sitos.v1"}), "zenoh/bytes;sitos.v1");
  EXPECT_EQ(BuildWireEncoding({"zenoh/bytes;sitos.v1"}), "zenoh/bytes;sitos.v1");
  EXPECT_EQ(BuildWireEncoding({std::string(sitos::Encoding::kSitosV1Batch)}),
            "zenoh/bytes;sitos.v1.batch");
}

TEST(ZenohEncodingTest, NormalizesCompatibleSitosWireEncodings) {
  using sitos::transport_test_access::NormalizeWireEncoding;

  EXPECT_EQ(NormalizeWireEncoding("zenoh/bytes;sitos.v1").id, sitos::Encoding::kSitosV1);
  EXPECT_EQ(NormalizeWireEncoding("zenoh.bytes;sitos.v1").id, sitos::Encoding::kSitosV1);
  EXPECT_EQ(NormalizeWireEncoding("sitos.v1").id, sitos::Encoding::kSitosV1);
  EXPECT_EQ(NormalizeWireEncoding("application/json").id, "application/json");
}

TEST(ZenohTransportStatusTest, GetUsesLatestConsolidation) {
  EXPECT_TRUE(sitos::transport_test_access::UsesLatestGetConsolidation());
}

TEST(ZenohTransportStatusTest, NativeCodesNeverUseAdapterDiagnostics) {
  constexpr std::array<std::int8_t, 3> kCollidingCodes = {-1, -2, -3};
  for (const auto code : kCollidingCodes) {
    const auto error = sitos::transport_test_access::MakeNativeError(code);
    EXPECT_STREQ(error.category().name(), "sitos.zenoh");
    EXPECT_EQ(error.value(), code);
    EXPECT_EQ(error.message(), "zenoh error code " + std::to_string(code));
  }
}

template <typename T>
void ExpectZenohSemanticError(const sitos::Result<T>& result, sitos::Status status,
                              std::string_view message, int cause_value) {
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), status);
  EXPECT_EQ(result.Message(), message);
  EXPECT_STREQ(result.Error().category().name(), "sitos.transport");
  EXPECT_EQ(result.Error().value(), cause_value);
  EXPECT_NE(result.Error().value(), 0);
}

TEST(ZenohTransportStatusTest, DisconnectedOperationsPreserveStatusMessageAndCause) {
  auto transport = sitos::transport_test_access::MakeDisconnectedTransport();
  ASSERT_NE(transport, nullptr);
  const std::vector<std::byte> payload = {std::byte{0x01}};
  const sitos::Encoding encoding{std::string(sitos::Encoding::kSitosV1)};

  ExpectZenohSemanticError(
      transport->Put("sitos/test/status/disconnected", payload, encoding, {}),
      sitos::Status::Disconnected, "zenoh session is not available", -1);
  ExpectZenohSemanticError(transport->Delete("sitos/test/status/disconnected", {}),
                           sitos::Status::Disconnected, "zenoh session is not available", -1);
  ExpectZenohSemanticError(
      transport->Get(
          "sitos/test/status/disconnected",
          [](std::string_view, std::span<const std::byte>, const sitos::Encoding&) {
            return true;
          },
          std::chrono::milliseconds(1)),
      sitos::Status::Disconnected, "zenoh session is not available", -1);
  ExpectZenohSemanticError(
      transport->DeclareSubscriber("sitos/test/status/disconnected",
                                   [](const sitos::TransportSample&) {}),
      sitos::Status::Disconnected, "zenoh session is not available", -1);
  ExpectZenohSemanticError(
      transport->DeclareQueryable("sitos/test/status/disconnected",
                                  [](sitos::TransportQuery&) {}),
      sitos::Status::Disconnected, "zenoh session is not available", -1);
}

TEST(ZenohTransportStatusTest, DeadQueryPreservesStatusMessageAndCause) {
  sitos::TransportQuery query;
  ExpectZenohSemanticError(query.Reply("sitos/test/status/dead-query", {},
                                       {std::string(sitos::Encoding::kSitosV1)}),
                           sitos::Status::Error,
                           "query is no longer valid (queryable destroyed)", -3);
}

class TransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = sitos::MakeZenohTransport();
    ASSERT_TRUE(transport_) << "Failed to open zenoh session";
  }

  void TearDown() override { transport_.reset(); }

  std::unique_ptr<sitos::Transport> transport_;
};

TEST(ZenohTransportStatusTest, ZeroTimeoutPrecedesDisconnectedSession) {
  auto transport = sitos::transport_test_access::MakeDisconnectedTransport();
  ASSERT_NE(transport, nullptr);

  const auto result = transport->Get(
      "sitos/test/status/zero-timeout-disconnected",
      [](std::string_view, std::span<const std::byte>, const sitos::Encoding&) { return true; },
      std::chrono::milliseconds::zero());

  ExpectZenohSemanticError(result, sitos::Status::InvalidArgument, "invalid argument", -2);
}

TEST(ZenohTransportStatusTest, InvalidArgumentsPreserveStatusMessageAndCause) {
  auto transport = sitos::MakeZenohTransport();
  ASSERT_NE(transport, nullptr) << "Failed to open zenoh session";

  const auto zero_timeout_result = transport->Get(
      "sitos/test/status/zero-timeout",
      [](std::string_view, std::span<const std::byte>, const sitos::Encoding&) { return true; },
      std::chrono::milliseconds::zero());
  ExpectZenohSemanticError(zero_timeout_result, sitos::Status::InvalidArgument, "invalid argument",
                           -2);

  const auto timeout_result = transport->Get(
      "sitos/test/status/invalid-timeout",
      [](std::string_view, std::span<const std::byte>, const sitos::Encoding&) { return true; },
      std::chrono::milliseconds(-1));
  ExpectZenohSemanticError(timeout_result, sitos::Status::InvalidArgument, "invalid argument",
                           -2);

  const auto subscriber_result =
      transport->DeclareSubscriber("sitos/test/status/empty-subscriber", {});
  ExpectZenohSemanticError(subscriber_result, sitos::Status::InvalidArgument, "invalid argument",
                           -2);

  const auto queryable_result =
      transport->DeclareQueryable("sitos/test/status/empty-queryable", {});
  ExpectZenohSemanticError(queryable_result, sitos::Status::InvalidArgument, "invalid argument",
                           -2);
}

TEST(ZenohTransportStatusTest, NativeKeyExpressionFailureRemainsError) {
  auto transport = sitos::MakeZenohTransport();
  ASSERT_NE(transport, nullptr) << "Failed to open zenoh session";

  const std::vector<std::byte> payload = {std::byte{0x01}};
  const auto result = transport->Put("sitos//invalid", payload,
                                      {std::string(sitos::Encoding::kSitosV1)}, {});
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  EXPECT_TRUE(result.Message().empty());
  EXPECT_STREQ(result.Error().category().name(), "sitos.zenoh");
  EXPECT_EQ(result.Error().value(), -1);
  EXPECT_EQ(result.Error().message(), "zenoh error code -1");
}

TEST_F(TransportTest, PutAndDeleteReturnOk) {
  std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0x02}};
  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);

  auto result = transport_->Put("sitos/test/key", payload, enc, {});
  ASSERT_TRUE(result.IsOk()) << "Put failed: " << result.Error().message();

  auto del_result = transport_->Delete("sitos/test/key", {});
  ASSERT_TRUE(del_result.IsOk()) << "Delete failed: " << del_result.Error().message();
}

TEST_F(TransportTest, GetWithoutQueryableDoesNotInvokeSink) {
  // With no matching queryable registered, Get should complete without
  // crashing and the sink should not be invoked.
  bool callback_called = false;
  auto result = transport_->Get(
      "sitos/test/**",
      [&](std::string_view /*key*/, std::span<const std::byte> /*payload*/,
          const sitos::Encoding& /*enc*/) {
        callback_called = true;
        return true;
      },
      std::chrono::milliseconds(500));

  EXPECT_TRUE(result.IsOk());
  EXPECT_FALSE(callback_called)
      << "No queryable is registered, so the sink should not be invoked";
}

TEST_F(TransportTest, SinkFalseSuppressesLaterReplies) {
  const std::string kSelector = "sitos/test/query/sink-false/**";
  const std::vector<std::byte> kPayload = {std::byte{0x01}};
  const sitos::Encoding kEncoding{std::string(sitos::Encoding::kSitosV1)};
  int sink_calls = 0;

  auto queryable = transport_->DeclareQueryable(kSelector, [&](sitos::TransportQuery& query) {
    EXPECT_TRUE(query.Reply("sitos/test/query/sink-false/one", kPayload, kEncoding).IsOk());
    EXPECT_TRUE(query.Reply("sitos/test/query/sink-false/two", kPayload, kEncoding).IsOk());
  });
  ASSERT_TRUE(queryable.IsOk()) << queryable.Error().message();

  const auto result = transport_->Get(
      kSelector,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&) {
        ++sink_calls;
        return false;
      },
      std::chrono::milliseconds(1000));

  EXPECT_TRUE(result.IsOk()) << result.Error().message();
  EXPECT_EQ(sink_calls, 1);
}

TEST_F(TransportTest, GetWaitsForTerminalCompletion) {
  const std::string kQueryKey = "sitos/test/query/terminal-completion";
  const std::vector<std::byte> kPayload = {std::byte{0x01}};
  const sitos::Encoding kEncoding{std::string(sitos::Encoding::kSitosV1)};
  std::mutex mutex;
  std::condition_variable condition;
  bool query_entered = false;
  bool release_query = false;
  bool get_returned = false;
  bool sink_called = false;
  sitos::Result<void> get_result = sitos::Result<void>::Err(sitos::Status::Error);

  auto queryable = transport_->DeclareQueryable(kQueryKey, [&](sitos::TransportQuery& query) {
    std::unique_lock<std::mutex> lock(mutex);
    query_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_query; });
    lock.unlock();
    EXPECT_TRUE(query.Reply(kQueryKey, kPayload, kEncoding).IsOk());
  });
  ASSERT_TRUE(queryable.IsOk()) << queryable.Error().message();

  std::thread get_thread([&] {
    auto result = transport_->Get(
        kQueryKey,
        [&](std::string_view, std::span<const std::byte> payload, const sitos::Encoding&) {
          std::lock_guard<std::mutex> lock(mutex);
          sink_called = std::vector<std::byte>(payload.begin(), payload.end()) == kPayload;
          return true;
        },
        std::chrono::milliseconds(2000));
    {
      std::lock_guard<std::mutex> lock(mutex);
      get_result = std::move(result);
      get_returned = true;
    }
    condition.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return query_entered; }));
    EXPECT_FALSE(get_returned);
    release_query = true;
  }
  condition.notify_all();
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return get_returned; }));
  }
  get_thread.join();

  EXPECT_TRUE(get_result.IsOk()) << get_result.Error().message();
  EXPECT_TRUE(sink_called);
}

TEST_F(TransportTest, QueryableRoundTrip) {
  // Declare a queryable that replies with a known payload, then query it.
  const std::string kQueryKey = "sitos/test/query/roundtrip";
  const std::vector<std::byte> kExpectedPayload = {
      std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);

  auto q = transport_->DeclareQueryable(
      kQueryKey,
      [&](sitos::TransportQuery& tq) {
        auto reply_result = tq.Reply(kQueryKey, kExpectedPayload, enc);
        EXPECT_TRUE(reply_result.IsOk())
            << "Queryable reply failed: " << reply_result.Error().message();
      });
  ASSERT_TRUE(q.IsOk()) << q.Error().message();

  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  std::vector<std::byte> received_payload;

  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view /*key*/, std::span<const std::byte> payload,
          const sitos::Encoding& /*enc*/) {
        {
          std::lock_guard<std::mutex> lock(mtx);
          received_payload.assign(payload.begin(), payload.end());
          done = true;
        }
        cv.notify_one();
        return false;  // stop after first result
      },
      std::chrono::milliseconds(2000));

  EXPECT_TRUE(result.IsOk()) << "Get failed: " << result.Error().message();

  {
    std::unique_lock<std::mutex> lock(mtx);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(3),
                            [&] { return done; }))
        << "Timed out waiting for queryable reply";
  }

  EXPECT_EQ(received_payload, kExpectedPayload);
}

TEST_F(TransportTest, QueryReplyPreservesActualEncoding) {
  const std::string kQueryKey = "sitos/test/query/encoding";
  const std::vector<std::byte> kPayload = {std::byte{0x42}};
  const sitos::Encoding kEncoding{"application/json"};
  std::mutex mutex;
  std::condition_variable condition;
  std::string received_encoding;

  auto queryable = transport_->DeclareQueryable(kQueryKey, [&](sitos::TransportQuery& query) {
    EXPECT_TRUE(query.Reply(kQueryKey, kPayload, kEncoding).IsOk());
  });
  ASSERT_TRUE(queryable.IsOk()) << queryable.Error().message();

  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding& encoding) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received_encoding = encoding.id;
        }
        condition.notify_one();
        return false;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3),
                                 [&] { return !received_encoding.empty(); }));
  EXPECT_EQ(received_encoding, kEncoding.id);
}

TEST_F(TransportTest, QueryReplyNormalizesLegacySitosEncoding) {
  const std::string kQueryKey = "sitos/test/query/legacy_encoding";
  const std::vector<std::byte> kPayload = {std::byte{0x42}};
  const sitos::Encoding kLegacyEncoding{"zenoh.bytes;sitos.v1"};
  std::mutex mutex;
  std::condition_variable condition;
  std::string received_encoding;

  auto queryable = transport_->DeclareQueryable(kQueryKey, [&](sitos::TransportQuery& query) {
    EXPECT_TRUE(query.Reply(kQueryKey, kPayload, kLegacyEncoding).IsOk());
  });
  ASSERT_TRUE(queryable.IsOk()) << queryable.Error().message();

  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding& encoding) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received_encoding = encoding.id;
        }
        condition.notify_one();
        return false;
      },
      std::chrono::milliseconds(2000));

  ASSERT_TRUE(result.IsOk());
  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3),
                                 [&] { return !received_encoding.empty(); }));
  EXPECT_EQ(received_encoding, sitos::Encoding::kSitosV1);
}

// A throwing user callback must not let a C++ exception cross the C ABI
// boundary of the zenoh-c queryable closure. The process must survive and
// the session must remain usable afterward.
TEST_F(TransportTest, QueryableCallbackExceptionIsContained) {
  const std::string kQueryKey = "sitos/test/query/throws";
  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);
  std::mutex callback_mutex;
  std::condition_variable callback_cv;
  bool callback_entered = false;
  std::atomic<bool> sink_called{false};

  auto q = transport_->DeclareQueryable(kQueryKey, [&](sitos::TransportQuery& /*tq*/) {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callback_entered = true;
    }
    callback_cv.notify_one();
    throw std::runtime_error("boom");
  });
  ASSERT_TRUE(q.IsOk()) << q.Error().message();

  // The queryable will be invoked but throws before replying. Wait until it
  // starts before checking that no reply reached the sink.
  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&) {
        sink_called.store(true);
        return true;
      },
      std::chrono::milliseconds(1000));

  EXPECT_TRUE(result.IsOk());
  {
    std::unique_lock<std::mutex> lock(callback_mutex);
    EXPECT_TRUE(callback_cv.wait_for(lock, std::chrono::seconds(3),
                                     [&] { return callback_entered; }));
  }
  EXPECT_FALSE(sink_called.load())
      << "The queryable threw before replying, so no reply is expected";

  // Prove the session is still healthy by round-tripping a working queryable.
  const std::string kOkKey = "sitos/test/query/ok_after_throw";
  const std::vector<std::byte> kPayload = {std::byte{0x01}};
  std::mutex reply_mutex;
  std::condition_variable reply_cv;
  bool reply_received = false;
  auto q2 = transport_->DeclareQueryable(
      kOkKey, [&](sitos::TransportQuery& tq) { tq.Reply(kOkKey, kPayload, enc); });
  ASSERT_TRUE(q2.IsOk()) << q2.Error().message();

  auto healthy_get = transport_->Get(
      kOkKey,
      [&](std::string_view, std::span<const std::byte> payload, const sitos::Encoding&) {
        {
          std::lock_guard<std::mutex> lock(reply_mutex);
          std::vector<std::byte> copy(payload.begin(), payload.end());
          if (copy == kPayload) reply_received = true;
        }
        reply_cv.notify_one();
        return false;
      },
      std::chrono::milliseconds(2000));
  EXPECT_TRUE(healthy_get.IsOk());
  std::unique_lock<std::mutex> lock(reply_mutex);
  EXPECT_TRUE(reply_cv.wait_for(lock, std::chrono::seconds(3),
                                [&] { return reply_received; }))
      << "Session should remain usable after a contained callback exception";
}

// A throwing Get result sink must not let a C++ exception cross the C ABI
// boundary of the zenoh-c reply closure.
TEST_F(TransportTest, GetSinkExceptionIsContained) {
  const std::string kQueryKey = "sitos/test/sink/throws";
  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);
  const std::vector<std::byte> kPayload = {std::byte{0x42}};
  std::mutex sink_mutex;
  std::condition_variable sink_cv;
  bool sink_entered = false;

  auto q = transport_->DeclareQueryable(
      kQueryKey, [&](sitos::TransportQuery& tq) { tq.Reply(kQueryKey, kPayload, enc); });
  ASSERT_TRUE(q.IsOk()) << q.Error().message();

  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&)
          -> bool {
        {
          std::lock_guard<std::mutex> lock(sink_mutex);
          sink_entered = true;
        }
        sink_cv.notify_one();
        throw std::runtime_error("sink boom");
      },
      std::chrono::milliseconds(2000));

  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  {
    std::unique_lock<std::mutex> lock(sink_mutex);
    EXPECT_TRUE(sink_cv.wait_for(lock, std::chrono::seconds(3),
                                 [&] { return sink_entered; }))
        << "The throwing sink must be invoked";
  }

  // Prove the session remains usable after the sink exception is contained.
  std::mutex reply_mutex;
  std::condition_variable reply_cv;
  bool reply_received = false;
  auto healthy_get = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte> payload, const sitos::Encoding&) {
        {
          std::lock_guard<std::mutex> lock(reply_mutex);
          reply_received = std::vector<std::byte>(payload.begin(), payload.end()) == kPayload;
        }
        reply_cv.notify_one();
        return false;
      },
      std::chrono::milliseconds(2000));

  EXPECT_TRUE(healthy_get.IsOk());
  std::unique_lock<std::mutex> lock(reply_mutex);
  EXPECT_TRUE(reply_cv.wait_for(lock, std::chrono::seconds(3),
                                [&] { return reply_received; }))
      << "Session should remain usable after a contained sink exception";
}

// An empty queryable callback must have defined, safe behavior: no crash on
// declaration or destruction, and the session remains usable.
TEST_F(TransportTest, EmptyQueryableCallbackIsSafe) {
  auto q = transport_->DeclareQueryable("sitos/test/empty_cb", {});
  EXPECT_FALSE(q.IsOk());
  // Destruction of the (empty) handle must not crash; it goes out of scope.
}

}  // namespace
