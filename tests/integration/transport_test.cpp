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
#include <string>
#include <vector>

namespace {

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

TEST_F(TransportTest, GetCompletesWithoutCrash) {
  // Even without a matching queryable, Get should complete without crashing.
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
  // No assertion on callback_called — may or may not receive results.
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
        tq.Reply(kQueryKey, kExpectedPayload, enc);
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

}  // namespace
