// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool WaitFor(std::function<bool()> predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

bool HasValue(const std::shared_ptr<sitos::InMemoryEngine>& engine, std::string_view key,
              std::span<const std::byte> expected) {
  bool found = false;
  engine->Get(key, [&](std::string_view, sitos::Bytes value) {
    found = std::vector<std::byte>(value.begin(), value.end()) ==
            std::vector<std::byte>(expected.begin(), expected.end());
    return true;
  });
  return found;
}

struct ReplyState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::byte> payload;
  std::string key;
  std::string encoding;
  int count = 0;
};

struct CapturedLogRecord {
  sitos::LogLevel level;
  std::string component;
  std::string message;
};

class CaptureSink final : public sitos::LogSink {
 public:
  void Write(const sitos::LogRecord& record) override {
    std::lock_guard lock(mutex);
    records.push_back(
        {.level = record.level,
         .component = std::string(record.component),
         .message = std::string(record.message)});
  }

  bool HasWarning(std::string_view message) const {
    std::lock_guard lock(mutex);
    for (const auto& record : records) {
      if (record.level == sitos::LogLevel::kWarning && record.component == "node" &&
          record.message == message) {
        return true;
      }
    }
    return false;
  }

 private:
  mutable std::mutex mutex;
  std::vector<CapturedLogRecord> records;
};

class StorageNodeSubscriberIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = sitos::MakeZenohTransport();
    ASSERT_TRUE(transport_);
    engine_ = std::make_shared<sitos::InMemoryEngine>();
    sink_ = std::make_shared<CaptureSink>();
    node_ = std::make_unique<sitos::StorageNode>();
    ASSERT_TRUE(node_->Start(engine_, *transport_,
                             {.prefix = "sitos/subscriber_test", .log_sink = sink_})
                    .IsOk());
  }

  void TearDown() override {
    node_.reset();
    transport_.reset();
  }

  std::unique_ptr<sitos::Transport> transport_;
  std::shared_ptr<sitos::InMemoryEngine> engine_;
  std::shared_ptr<CaptureSink> sink_;
  std::unique_ptr<sitos::StorageNode> node_;
};

TEST_F(StorageNodeSubscriberIntegrationTest, PutGetAndDeleteRoundTrip) {
  const std::vector<std::byte> expected = {std::byte{0x00}, std::byte{0xA5}};
  ASSERT_TRUE(transport_
                  ->Put("sitos/subscriber_test/base/value", expected,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  ASSERT_TRUE(WaitFor([&] { return HasValue(engine_, "value", expected); }));

  auto reply = std::make_shared<ReplyState>();
  ASSERT_TRUE(transport_
                  ->Get("sitos/subscriber_test/base/value",
                        [reply](std::string_view key, std::span<const std::byte> payload,
                                sitos::Encoding encoding) {
                          std::lock_guard lock(reply->mutex);
                          reply->key = std::string(key);
                          reply->payload.assign(payload.begin(), payload.end());
                          reply->encoding = std::move(encoding.id);
                          ++reply->count;
                          reply->condition.notify_all();
                          return false;
                        },
                        2000ms)
                  .IsOk());
  {
    std::unique_lock lock(reply->mutex);
    ASSERT_TRUE(reply->condition.wait_for(lock, 3s, [&] { return reply->count == 1; }));
  }
  EXPECT_EQ(reply->key, "sitos/subscriber_test/base/value");
  EXPECT_EQ(reply->payload, expected);
  EXPECT_EQ(reply->encoding, sitos::Encoding::kSitosV1);

  ASSERT_TRUE(transport_->Delete("sitos/subscriber_test/base/value", {}).IsOk());
  ASSERT_TRUE(WaitFor([&] {
    return !engine_->Get("value", [](std::string_view, sitos::Bytes) { return true; });
  }));

  auto after_delete = std::make_shared<ReplyState>();
  ASSERT_TRUE(transport_
                  ->Get("sitos/subscriber_test/base/value",
                        [after_delete](std::string_view, std::span<const std::byte>,
                                       sitos::Encoding) {
                          std::lock_guard lock(after_delete->mutex);
                          ++after_delete->count;
                          after_delete->condition.notify_all();
                          return true;
                        },
                        500ms)
                  .IsOk());
  std::unique_lock lock(after_delete->mutex);
  EXPECT_FALSE(after_delete->condition.wait_for(lock, 700ms,
                                                [&] { return after_delete->count != 0; }));
}

TEST_F(StorageNodeSubscriberIntegrationTest, UnknownEncodingIsStoredAsPayloadV1Bytes) {
  const std::vector<std::byte> raw = {std::byte{0x10}, std::byte{0x00}, std::byte{0xFF}};
  const std::vector<std::byte> expected = {std::byte{0x04}, std::byte{0x10},
                                           std::byte{0x00}, std::byte{0xFF}};
  ASSERT_TRUE(transport_
                  ->Put("sitos/subscriber_test/base/opaque", raw,
                        sitos::Encoding{"application/octet-stream"}, {})
                  .IsOk());
  ASSERT_TRUE(WaitFor([&] { return HasValue(engine_, "opaque", expected); }));
  EXPECT_TRUE(sink_->HasWarning("unknown subscriber encoding; wrapped as bytes"));

  auto reply = std::make_shared<ReplyState>();
  ASSERT_TRUE(transport_
                  ->Get("sitos/subscriber_test/base/opaque",
                        [reply](std::string_view key, std::span<const std::byte> payload,
                                sitos::Encoding encoding) {
                          std::lock_guard lock(reply->mutex);
                          reply->key = std::string(key);
                          reply->payload.assign(payload.begin(), payload.end());
                          reply->encoding = std::move(encoding.id);
                          ++reply->count;
                          reply->condition.notify_all();
                          return false;
                        },
                        2000ms)
                  .IsOk());
  {
    std::unique_lock lock(reply->mutex);
    ASSERT_TRUE(reply->condition.wait_for(lock, 3s, [&] { return reply->count == 1; }));
  }
  EXPECT_EQ(reply->key, "sitos/subscriber_test/base/opaque");
  EXPECT_EQ(reply->payload, expected);
  EXPECT_EQ(reply->encoding, sitos::Encoding::kSitosV1);
}

TEST_F(StorageNodeSubscriberIntegrationTest, SnapPutIsIgnoredAfterSamePublisherBarrier) {
  const std::vector<std::byte> existing = {std::byte{0x11}};
  const std::vector<std::byte> snap_payload = {std::byte{0x22}};
  ASSERT_TRUE(engine_->Put("existing", existing));
  ASSERT_TRUE(transport_
                  ->Put("sitos/subscriber_test/snap/session1/ignored", snap_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  const std::vector<std::byte> barrier = {std::byte{0x33}};
  ASSERT_TRUE(transport_
                  ->Put("sitos/subscriber_test/base/barrier", barrier,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  ASSERT_TRUE(WaitFor([&] { return HasValue(engine_, "barrier", barrier); }));
  EXPECT_TRUE(sink_->HasWarning("read-only snapshot key"));

  EXPECT_FALSE(engine_->Get("session1/ignored", [](std::string_view, sitos::Bytes) {
    return true;
  }));
  EXPECT_TRUE(HasValue(engine_, "existing", existing));
}

}  // namespace
