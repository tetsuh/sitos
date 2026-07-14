// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end batch delivery over one same-process zenoh session.

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"
#include "sitos/param_value.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct ObservedSample {
  std::string key;
  std::vector<std::byte> payload;
  std::string encoding;
  int count = 0;
};

class CaptureSink final : public sitos::LogSink {
 public:
  void Write(const sitos::LogRecord& record) override {
    std::lock_guard lock(mutex_);
    messages_.emplace_back(record.message);
  }

  std::vector<std::string> Messages() const {
    std::lock_guard lock(mutex_);
    return messages_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> messages_;
};

std::optional<std::vector<std::byte>> GetOne(sitos::Transport& transport,
                                             std::string_view key) {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<std::vector<std::byte>> payload;
  if (!transport
           .Get(key,
                [&](std::string_view, std::span<const std::byte> value, sitos::Encoding) {
                  std::lock_guard lock(mutex);
                  payload = std::vector<std::byte>(value.begin(), value.end());
                  cv.notify_all();
                  return false;
                },
                200ms)
           .IsOk()) {
    return std::nullopt;
  }
  std::unique_lock lock(mutex);
  if (!cv.wait_for(lock, 300ms, [&] { return payload.has_value(); })) return std::nullopt;
  return payload;
}

// A transport Put only confirms submission. Retry exact queries until the
// subscriber has applied the batch to the selected overlay.
std::optional<std::vector<std::byte>> GetOneEventually(sitos::Transport& transport,
                                                        std::string_view key) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (auto payload = GetOne(transport, key)) return payload;
  }
  return std::nullopt;
}

}  // namespace

TEST(BatchIntegrationTest, BatchIsReceivedBySessionSubscriber) {
  auto transport = sitos::MakeZenohTransport();
  ASSERT_TRUE(transport);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node;
  auto log_sink = std::make_shared<CaptureSink>();
  constexpr std::string_view kPrefix = "sitos/batch_test";
  ASSERT_TRUE(node.Start(engine, *transport,
                         {.prefix = std::string(kPrefix), .log_sink = log_sink})
                  .IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  auto observed = std::make_shared<ObservedSample>();
  auto mutex = std::make_shared<std::mutex>();
  auto cv = std::make_shared<std::condition_variable>();
  auto observer_result = transport->DeclareSubscriber(
      "sitos/batch_test/session/s1/**",
      [observed, mutex, cv](const sitos::TransportSample& sample) {
        std::lock_guard lock(*mutex);
        observed->key = sample.key;
        observed->payload.assign(sample.payload.begin(), sample.payload.end());
        observed->encoding = sample.encoding.id;
        ++observed->count;
        cv->notify_all();
      });
  ASSERT_TRUE(observer_result.IsOk());
  sitos::Subscription observer = std::move(observer_result).Value();

  const std::vector<std::pair<std::string, sitos::ParamValue>> entries = {
      {"first", sitos::ParamValue(std::int64_t{1})},
      {"second", sitos::ParamValue("two")},
  };
  const auto batch = sitos::EncodeBatch(entries);
  auto put_result = transport->Put(
      "sitos/batch_test/session/s1/:batch", batch,
      sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)}, {});
  ASSERT_TRUE(put_result.IsOk()) << put_result.Error().message();

  auto first = GetOneEventually(*transport, "sitos/batch_test/session/s1/first");
  ASSERT_TRUE(first.has_value()) << [&] {
    std::string diagnostics;
    for (const auto& message : log_sink->Messages()) {
      if (!diagnostics.empty()) diagnostics.append(", ");
      diagnostics.append(message);
    }
    return diagnostics;
  }();

  {
    std::unique_lock lock(*mutex);
    ASSERT_TRUE(cv->wait_for(lock, 3s, [&] { return observed->count == 1; }));
    EXPECT_EQ(observed->key, "sitos/batch_test/session/s1/:batch");
    EXPECT_EQ(observed->payload, batch);
    EXPECT_EQ(observed->encoding, sitos::Encoding::kSitosV1Batch);
  }

  auto first_value = sitos::ParamValue::Decode(*first);
  ASSERT_TRUE(first_value.has_value());
  EXPECT_EQ(first_value->As<std::int64_t>(), 1);
  auto second = GetOneEventually(*transport, "sitos/batch_test/session/s1/second");
  ASSERT_TRUE(second.has_value());
  auto second_value = sitos::ParamValue::Decode(*second);
  ASSERT_TRUE(second_value.has_value());
  EXPECT_EQ(second_value->As<std::string>(), "two");

  observer = sitos::Subscription{};
  node.Stop();
}
