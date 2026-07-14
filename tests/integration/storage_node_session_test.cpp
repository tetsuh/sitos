// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end session management over a same-process zenoh session: snapshot
// isolation, overlay round-trip, and release on CloseSession.

#include "sitos/in_memory_engine.hpp"
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
#include <vector>

namespace {

using namespace std::chrono_literals;

sitos::Encoding V1() { return sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}; }

// Puts a value and waits until a get observes the expected bytes, confirming the
// StorageNode has applied it. Returns false on timeout.
bool PutAndConfirm(sitos::Transport& transport, std::string_view key,
                   const std::vector<std::byte>& value) {
  if (!transport.Put(key, value, V1(), {}).IsOk()) return false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    std::mutex mutex;
    std::condition_variable cv;
    bool matched = false;
    if (!transport
             .Get(key,
                  [&](std::string_view, std::span<const std::byte> payload, sitos::Encoding) {
                    if (std::vector<std::byte>(payload.begin(), payload.end()) == value) {
                      std::lock_guard lock(mutex);
                      matched = true;
                      cv.notify_one();
                    }
                    return false;
                  },
                  1s)
             .IsOk()) {
      return false;
    }
    std::unique_lock lock(mutex);
    if (cv.wait_for(lock, 200ms, [&] { return matched; })) return true;
  }
  return false;
}

// Collects the single-reply payload for an exact get, or nullopt if no reply
// arrives within the window.
std::optional<std::vector<std::byte>> GetOne(sitos::Transport& transport, std::string_view key) {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<std::vector<std::byte>> result;
  if (!transport
           .Get(key,
                [&](std::string_view, std::span<const std::byte> payload, sitos::Encoding) {
                  std::lock_guard lock(mutex);
                  result = std::vector<std::byte>(payload.begin(), payload.end());
                  cv.notify_one();
                  return false;
                },
                1s)
           .IsOk()) {
    return std::nullopt;
  }
  std::unique_lock lock(mutex);
  cv.wait_for(lock, 1s, [&] { return result.has_value(); });
  return result;
}

// Confirms an exact get yields no reply within a short window.
bool GetIsEmpty(sitos::Transport& transport, std::string_view key) {
  std::mutex mutex;
  std::condition_variable cv;
  bool received = false;
  if (!transport
           .Get(key,
                [&](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                  std::lock_guard lock(mutex);
                  received = true;
                  cv.notify_one();
                  return false;
                },
                500ms)
           .IsOk()) {
    return false;
  }
  std::unique_lock lock(mutex);
  return !cv.wait_for(lock, 700ms, [&] { return received; });
}

}  // namespace

TEST(StorageNodeSessionIntegrationTest, SnapshotOverlayRoundTripAndClose) {
  auto transport = sitos::MakeZenohTransport();
  ASSERT_TRUE(transport);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node;
  ASSERT_TRUE(node.Start(engine, *transport, {.prefix = "sitos/session_test"}).IsOk());

  const std::vector<std::byte> before = {std::byte{0x11}};
  const std::vector<std::byte> after = {std::byte{0x22}};

  // Confirm the pre-snapshot base write, then open the session.
  ASSERT_TRUE(PutAndConfirm(*transport, "sitos/session_test/base/value", before));
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  // A later base write must not change the snapshot.
  ASSERT_TRUE(PutAndConfirm(*transport, "sitos/session_test/base/value", after));

  auto snap = GetOne(*transport, "sitos/session_test/snap/s1/value");
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(*snap, before);

  // Overlay writes round-trip and are isolated from base and snapshot.
  const std::vector<std::byte> overlay_value = {std::byte{0xAB}};
  ASSERT_TRUE(PutAndConfirm(*transport, "sitos/session_test/session/s1/p", overlay_value));
  auto session = GetOne(*transport, "sitos/session_test/session/s1/p");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(*session, overlay_value);
  EXPECT_TRUE(GetIsEmpty(*transport, "sitos/session_test/base/p"));

  // meta/session/<sid> is served while the session is active.
  EXPECT_TRUE(GetOne(*transport, "sitos/session_test/meta/session/s1").has_value());

  // After close, snapshot, overlay, and meta all stop replying.
  ASSERT_TRUE(node.CloseSession("s1").IsOk());
  EXPECT_TRUE(GetIsEmpty(*transport, "sitos/session_test/snap/s1/value"));
  EXPECT_TRUE(GetIsEmpty(*transport, "sitos/session_test/session/s1/p"));
  EXPECT_TRUE(GetIsEmpty(*transport, "sitos/session_test/meta/session/s1"));
  EXPECT_TRUE(node.ActiveSessions().empty());
}
