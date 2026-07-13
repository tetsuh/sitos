// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct ReplyState {
  std::mutex mutex;
  std::condition_variable condition;
  bool received = false;
};

}  // namespace

TEST(StorageNodeLifecycleIntegrationTest, StopQuiescesZenohDeclarations) {
  auto transport = sitos::MakeZenohTransport();
  ASSERT_TRUE(transport);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node;
  constexpr std::string_view kPrefix = "sitos/lifecycle_test";
  ASSERT_TRUE(node.Start(engine, *transport, {.prefix = std::string(kPrefix)}).IsOk());

  const std::vector<std::byte> payload = {std::byte{0x11}, std::byte{0x22}};
  ASSERT_TRUE(transport
                  ->Put("sitos/lifecycle_test/base/value", payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());

  ReplyState before_stop;
  ASSERT_TRUE(transport
                  ->Get("sitos/lifecycle_test/base/value",
                        [&](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                          {
                            std::lock_guard lock(before_stop.mutex);
                            before_stop.received = true;
                          }
                          before_stop.condition.notify_one();
                          return false;
                        },
                        2s)
                  .IsOk());
  {
    std::unique_lock lock(before_stop.mutex);
    ASSERT_TRUE(before_stop.condition.wait_for(lock, 3s,
                                               [&] { return before_stop.received; }));
  }

  node.Stop();
  EXPECT_FALSE(node.IsStarted());

  std::mutex barrier_mutex;
  std::condition_variable barrier_condition;
  bool barrier_received = false;
  auto barrier = transport->DeclareSubscriber(
      "sitos/lifecycle_test/barrier/**", [&](const sitos::TransportSample&) {
        {
          std::lock_guard lock(barrier_mutex);
          barrier_received = true;
        }
        barrier_condition.notify_one();
      });
  ASSERT_TRUE(barrier.IsOk());

  const std::vector<std::byte> after_stop_payload = {std::byte{0x33}};
  ASSERT_TRUE(transport
                  ->Put("sitos/lifecycle_test/base/value", after_stop_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  ASSERT_TRUE(transport
                  ->Put("sitos/lifecycle_test/barrier/done", {},
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {})
                  .IsOk());
  {
    std::unique_lock lock(barrier_mutex);
    ASSERT_TRUE(barrier_condition.wait_for(lock, 3s, [&] { return barrier_received; }));
  }

  bool unchanged = false;
  ASSERT_TRUE(engine->Get("value", [&](std::string_view, sitos::Bytes value) {
    unchanged = std::vector<std::byte>(value.begin(), value.end()) == payload;
    return true;
  }));
  EXPECT_TRUE(unchanged);

  ReplyState after_stop;
  ASSERT_TRUE(transport
                  ->Get("sitos/lifecycle_test/base/value",
                        [&](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                          {
                            std::lock_guard lock(after_stop.mutex);
                            after_stop.received = true;
                          }
                          after_stop.condition.notify_one();
                          return false;
                        },
                        500ms)
                  .IsOk());
  std::unique_lock lock(after_stop.mutex);
  EXPECT_FALSE(after_stop.condition.wait_for(lock, 1s, [&] { return after_stop.received; }));
}
