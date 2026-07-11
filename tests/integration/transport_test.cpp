// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the Transport abstraction using a real zenoh session.

#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

TEST(TransportApiTest, TransportQueryCannotOutliveCallback) {
  static_assert(!std::is_move_constructible_v<sitos::TransportQuery>);
  static_assert(!std::is_move_assignable_v<sitos::TransportQuery>);
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

// A throwing user callback must not let a C++ exception cross the C ABI
// boundary of the zenoh-c queryable closure. The process must survive and
// the session must remain usable afterward.
TEST_F(TransportTest, QueryableCallbackExceptionIsContained) {
  const std::string kQueryKey = "sitos/test/query/throws";
  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);

  auto q = transport_->DeclareQueryable(
      kQueryKey,
      [&](sitos::TransportQuery& /*tq*/) { throw std::runtime_error("boom"); });

  // The queryable will be invoked but throws before replying. Get must return
  // without crashing, and no reply should reach the sink.
  bool sink_called = false;
  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&) {
        sink_called = true;
        return true;
      },
      std::chrono::milliseconds(1000));

  EXPECT_TRUE(result.IsOk());
  EXPECT_FALSE(sink_called)
      << "The queryable threw before replying, so no reply is expected";

  // Prove the session is still healthy by round-tripping a working queryable.
  const std::string kOkKey = "sitos/test/query/ok_after_throw";
  const std::vector<std::byte> kPayload = {std::byte{0x01}};
  auto q2 = transport_->DeclareQueryable(
      kOkKey, [&](sitos::TransportQuery& tq) { tq.Reply(kOkKey, kPayload, enc); });

  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  transport_->Get(
      kOkKey,
      [&](std::string_view, std::span<const std::byte> payload, const sitos::Encoding&) {
        {
          std::lock_guard<std::mutex> lock(mtx);
          std::vector<std::byte> copy(payload.begin(), payload.end());
          if (copy == kPayload) done = true;
        }
        cv.notify_one();
        return false;
      },
      std::chrono::milliseconds(2000));
  std::unique_lock<std::mutex> lock(mtx);
  EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return done; }))
      << "Session should remain usable after a contained callback exception";
}

// A throwing Get result sink must not let a C++ exception cross the C ABI
// boundary of the zenoh-c reply closure.
TEST_F(TransportTest, GetSinkExceptionIsContained) {
  const std::string kQueryKey = "sitos/test/sink/throws";
  sitos::Encoding enc;
  enc.id = std::string(sitos::Encoding::kSitosV1);
  const std::vector<std::byte> kPayload = {std::byte{0x42}};

  auto q = transport_->DeclareQueryable(
      kQueryKey, [&](sitos::TransportQuery& tq) { tq.Reply(kQueryKey, kPayload, enc); });

  auto result = transport_->Get(
      kQueryKey,
      [&](std::string_view, std::span<const std::byte>, const sitos::Encoding&)
          -> bool {
        throw std::runtime_error("sink boom");
      },
      std::chrono::milliseconds(2000));

  EXPECT_TRUE(result.IsOk());
  // Reaching this assertion means the exception did not crash the process.
}

// An empty queryable callback must have defined, safe behavior: no crash on
// declaration or destruction, and the session remains usable.
TEST_F(TransportTest, EmptyQueryableCallbackIsSafe) {
  auto q = transport_->DeclareQueryable("sitos/test/empty_cb", {});
  // Destruction of the (empty) handle must not crash; it goes out of scope.
}

}  // namespace
