// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "transport/get_completion.hpp"

namespace {

using namespace std::chrono_literals;
using sitos::Encoding;
using sitos::Result;
using sitos::Status;
using sitos::transport_internal::GetCompletion;
using sitos::transport_internal::QueryReply;

class BlockingSink {
 public:
  explicit BlockingSink(bool result = true) : result_(result) {}

  bool operator()(std::string_view, std::span<const std::byte>, const Encoding&) {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    return result_;
  }

  void WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(condition_.wait_for(lock, 3s, [this] { return entered_; }));
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
  bool result_;
};

class CallbackEntered {
 public:
  void Signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(condition_.wait_for(lock, 3s, [this] { return entered_; }));
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
};

Result<QueryReply> Reply(std::string key, std::span<const std::byte> payload) {
  return Result<QueryReply>::Ok(
      QueryReply{std::move(key), payload, Encoding{std::string(Encoding::kSitosV1)}, nullptr});
}

template <typename Converter>
std::future<void> StartReply(const std::shared_ptr<GetCompletion>& completion,
                             CallbackEntered* entered, Converter converter) {
  return std::async(std::launch::async,
                    [completion, entered, converter = std::move(converter)]() mutable {
                      auto lease = completion->AcquireCallbackLease(completion);
                      if (entered != nullptr) entered->Signal();
                      completion->ProcessReply(converter);
                    });
}

TEST(TransportGetCompletionTest, WaitsForDropAndInFlightSink) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  BlockingSink sink;
  auto completion = std::make_shared<GetCompletion>(std::ref(sink));

  auto waiter =
      std::async(std::launch::async, [completion] { return completion->WaitForResult(); });
  auto callback = StartReply(completion, nullptr, [&] { return Reply("sitos/test/one", payload); });
  sink.WaitUntilEntered();

  completion->MarkDropped();
  EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);

  sink.Release();
  callback.get();
  EXPECT_TRUE(waiter.get().IsOk());
}

TEST(TransportGetCompletionTest, DropsLateCallbacksWithoutDelivery) {
  int sink_calls = 0;
  auto completion = std::make_shared<GetCompletion>(
      [&](std::string_view, std::span<const std::byte>, const Encoding&) {
        ++sink_calls;
        return true;
      });

  completion->MarkDropped();
  auto lease = completion->AcquireCallbackLease(completion);

  EXPECT_FALSE(lease.IsEnrolled());
  EXPECT_TRUE(completion->WaitForResult().IsOk());
  EXPECT_EQ(sink_calls, 0);
}

TEST(TransportGetCompletionTest, FalseSuppressesQueuedCallbacksAndReturnsOk) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  BlockingSink sink(false);
  auto completion = std::make_shared<GetCompletion>(std::ref(sink));
  CallbackEntered second_entered;
  int second_converter_calls = 0;

  auto first = StartReply(completion, nullptr, [&] { return Reply("sitos/test/first", payload); });
  sink.WaitUntilEntered();
  auto second = StartReply(completion, &second_entered, [&] {
    ++second_converter_calls;
    return Reply("sitos/test/second", payload);
  });
  second_entered.Wait();

  sink.Release();
  first.get();
  second.get();
  completion->MarkDropped();

  EXPECT_EQ(second_converter_calls, 0);
  EXPECT_TRUE(completion->WaitForResult().IsOk());
}

TEST(TransportGetCompletionTest, FalseStillWaitsForTerminalDrop) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  auto completion = std::make_shared<GetCompletion>(
      [](std::string_view, std::span<const std::byte>, const Encoding&) { return false; });

  {
    auto lease = completion->AcquireCallbackLease(completion);
    completion->ProcessReply([&] { return Reply("sitos/test/one", payload); });
  }
  auto waiter =
      std::async(std::launch::async, [completion] { return completion->WaitForResult(); });

  EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
  completion->MarkDropped();
  EXPECT_TRUE(waiter.get().IsOk());
}

TEST(TransportGetCompletionTest, ExceptionSuppressesQueuedCallbacksAndReturnsError) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  CallbackEntered second_entered;
  int first_calls = 0;
  int second_converter_calls = 0;
  auto completion = std::make_shared<GetCompletion>(
      [&](std::string_view, std::span<const std::byte>, const Encoding&) -> bool {
        ++first_calls;
        throw std::runtime_error("sink failure");
      });

  auto first = StartReply(completion, nullptr, [&] { return Reply("sitos/test/first", payload); });
  auto second = StartReply(completion, &second_entered, [&] {
    ++second_converter_calls;
    return Reply("sitos/test/second", payload);
  });
  second_entered.Wait();

  first.get();
  second.get();
  completion->MarkDropped();
  const auto result = completion->WaitForResult();

  EXPECT_EQ(first_calls, 1);
  EXPECT_EQ(second_converter_calls, 0);
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
}

TEST(TransportGetCompletionTest, DeduplicatesConcreteKeysAndRetainsDistinctKeys) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  std::vector<std::string> delivered;
  auto completion = std::make_shared<GetCompletion>(
      [&](std::string_view key, std::span<const std::byte>, const Encoding&) {
        delivered.emplace_back(key);
        return true;
      });

  for (const std::string_view key : {"sitos/test/one", "sitos/test/one", "sitos/test/two"}) {
    auto lease = completion->AcquireCallbackLease(completion);
    completion->ProcessReply([&] { return Reply(std::string(key), payload); });
  }
  completion->MarkDropped();

  EXPECT_TRUE(completion->WaitForResult().IsOk());
  EXPECT_EQ(delivered, (std::vector<std::string>{"sitos/test/one", "sitos/test/two"}));
}

TEST(TransportGetCompletionTest, PreservesFirstConversionFailure) {
  auto completion = std::make_shared<GetCompletion>(
      [](std::string_view, std::span<const std::byte>, const Encoding&) { return true; });
  const auto first_cause = std::make_error_code(std::errc::io_error);
  const auto second_cause = std::make_error_code(std::errc::invalid_argument);

  {
    auto lease = completion->AcquireCallbackLease(completion);
    completion->ProcessReply([&] {
      return Result<QueryReply>::Err(Status::Error, "first conversion failure", first_cause);
    });
  }
  {
    auto lease = completion->AcquireCallbackLease(completion);
    completion->ProcessReply([&] {
      return Result<QueryReply>::Err(Status::Error, "second conversion failure", second_cause);
    });
  }
  completion->MarkDropped();

  const auto result = completion->WaitForResult();
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.Error(), first_cause);
  EXPECT_EQ(result.Message(), "failed to process zenoh get reply");
}

TEST(TransportGetCompletionTest, RepeatedStateTeardownIsSafe) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  for (int index = 0; index < 100; ++index) {
    auto completion = std::make_shared<GetCompletion>(
        [](std::string_view, std::span<const std::byte>, const Encoding&) { return true; });
    {
      auto lease = completion->AcquireCallbackLease(completion);
      completion->ProcessReply([&] { return Reply("sitos/test/repeated", payload); });
    }
    completion->MarkDropped();
    EXPECT_TRUE(completion->WaitForResult().IsOk());
  }
}

TEST(TransportGetCompletionTest, IndependentRequestsInvokeSinksConcurrently) {
  std::vector<std::byte> payload = {std::byte{0x01}};
  BlockingSink first_sink;
  BlockingSink second_sink;
  auto first = std::make_shared<GetCompletion>(std::ref(first_sink));
  auto second = std::make_shared<GetCompletion>(std::ref(second_sink));

  auto first_callback =
      StartReply(first, nullptr, [&] { return Reply("sitos/test/first", payload); });
  auto second_callback =
      StartReply(second, nullptr, [&] { return Reply("sitos/test/second", payload); });
  first_sink.WaitUntilEntered();
  second_sink.WaitUntilEntered();

  first->MarkDropped();
  second->MarkDropped();
  first_sink.Release();
  second_sink.Release();
  first_callback.get();
  second_callback.get();

  EXPECT_TRUE(first->WaitForResult().IsOk());
  EXPECT_TRUE(second->WaitForResult().IsOk());
}

}  // namespace
