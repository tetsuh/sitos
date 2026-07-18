// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

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
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "param_cache_test_access.hpp"

namespace {

using sitos::BatchEntry;
using sitos::ClientConfig;
using sitos::Encoding;
using sitos::ParamCache;
using sitos::ParamValue;
using sitos::PutOptions;
using sitos::Queryable;
using sitos::Result;
using sitos::Subscription;
using sitos::Transport;
using sitos::TransportQuery;
using sitos::TransportSample;
using Access = sitos::param_cache_test_access::ParamCacheTestAccess;

class FakeTransport final : public Transport {
 public:
  struct Reply {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
  };

  Result<void> Put(std::string_view, std::span<const std::byte>, Encoding,
                   PutOptions) override {
    return Result<void>::Ok();
  }
  Result<void> Delete(std::string_view, PutOptions) override { return Result<void>::Ok(); }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds) override {
    std::function<void()> hook;
    std::vector<Reply> replies_copy;
    {
      std::lock_guard lock(mutex);
      calls.push_back("get:" + std::string(keyexpr));
      hook = get_hook;
      replies_copy = replies[std::string(keyexpr)];
    }
    if (hook) hook();
    for (const auto& reply : replies_copy) {
      if (!sink(reply.key, reply.payload, reply.encoding)) break;
    }
    return get_result;
  }

  Result<Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const TransportSample&)> callback) override {
    {
      std::lock_guard lock(mutex);
      calls.push_back("declare:" + std::string(keyexpr));
      subscriber = std::move(callback);
    }
    return std::move(declaration_result);
  }

  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)>) override {
    return Result<Queryable>::Ok(Queryable{});
  }

  void EmitOwned(std::string key, std::vector<std::byte> payload,
                 std::string encoding = std::string(Encoding::kSitosV1),
                 TransportSample::Kind kind = TransportSample::Kind::Put) {
    std::function<void(const TransportSample&)> callback;
    {
      std::lock_guard lock(mutex);
      callback = subscriber;
    }
    ASSERT_TRUE(callback);
    TransportSample sample{std::move(key), payload, Encoding{std::move(encoding)}, std::nullopt,
                           kind};
    callback(sample);
  }

  std::mutex mutex;
  std::vector<std::string> calls;
  std::unordered_map<std::string, std::vector<Reply>> replies;
  std::function<void()> get_hook;
  std::function<void(const TransportSample&)> subscriber;
  Result<Subscription> declaration_result = Result<Subscription>::Ok(Subscription{});
  Result<void> get_result = Result<void>::Ok();
};

std::vector<std::byte> Payload(const ParamValue& value) { return value.Encode(); }

TEST(ParamCacheTest, OpenValidationAndMovedFromBehavior) {
  auto null_result = ParamCache::Open(std::shared_ptr<Transport>{});
  ASSERT_FALSE(null_result.IsOk());
  EXPECT_EQ(null_result.StatusCode(), sitos::Status::InvalidArgument);

  auto transport = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.zenoh_config_json = "{mode: 'peer'}";
  auto injected = ParamCache::Open(transport, config);
  ASSERT_FALSE(injected.IsOk());
  EXPECT_EQ(injected.StatusCode(), sitos::Status::InvalidArgument);

  auto opened = ParamCache::Open(transport);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  auto cache = std::move(opened).Value();
  auto moved = std::move(cache);
  EXPECT_EQ(cache.Attach("s1").StatusCode(), sitos::Status::InvalidArgument);
  cache.Detach();
  EXPECT_FALSE(Access::IsAttached(moved));
}

TEST(ParamCacheTest, AttachUsesSubscriberFirstAndAppliesSnapshotOverlayAndBufferedDelta) {
  auto transport = std::make_shared<FakeTransport>();
  const auto snapshot_payload = Payload(ParamValue(std::int64_t{1}));
  const auto overlay_payload = Payload(ParamValue(std::int64_t{2}));
  transport->replies["sitos/snap/s1/**"] = {
      {"sitos/snap/s1/inherited", snapshot_payload, Encoding{std::string(Encoding::kSitosV1)}}};
  transport->replies["sitos/session/s1/**"] = {
      {"sitos/session/s1/overlaid", overlay_payload, Encoding{std::string(Encoding::kSitosV1)}}};
  bool emitted = false;
  transport->get_hook = [&] {
    if (!emitted) {
      emitted = true;
      transport->EmitOwned("sitos/session/s1/inherited", Payload(ParamValue(3)));
    }
  };

  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_EQ(transport->calls.size(), 3U);
  EXPECT_TRUE(transport->calls[0].starts_with("declare:sitos/session/s1/**"));
  EXPECT_EQ(Access::Get(cache, "inherited")->As<std::int64_t>(), 3);
  EXPECT_EQ(Access::Get(cache, "overlaid")->As<std::int64_t>(), 2);
}

TEST(ParamCacheTest, AttachDoesNotMissConcurrentPut) {
  auto transport = std::make_shared<FakeTransport>();
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  transport->get_hook = [&] {
    std::unique_lock lock(mutex);
    if (entered) return;
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  };

  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  std::thread attach([&] { EXPECT_TRUE(cache.Attach("s1").IsOk()); });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
  }
  transport->EmitOwned("sitos/session/s1/concurrent", Payload(ParamValue(7)));
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  attach.join();
  ASSERT_TRUE(Access::Get(cache, "concurrent").has_value());
  EXPECT_EQ(Access::Get(cache, "concurrent")->As<std::int64_t>(), 7);
}

TEST(ParamCacheTest, SessionDeleteRestoresSnapshotAndBatchIsAllOrNothing) {
  auto transport = std::make_shared<FakeTransport>();
  transport->replies["sitos/snap/s1/**"] = {
      {"sitos/snap/s1/key", Payload(ParamValue(1)), Encoding{std::string(Encoding::kSitosV1)}}};
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->EmitOwned("sitos/session/s1/key", Payload(ParamValue(2)));
  ASSERT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 2);
  transport->EmitOwned("sitos/session/s1/key", {}, "ignored", TransportSample::Kind::Delete);
  ASSERT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 1);

  const std::vector<BatchEntry> entries = {{"a", ParamValue(3)}, {"a", ParamValue(4)}};
  auto batch = sitos::EncodeBatch(entries);
  transport->EmitOwned("sitos/session/s1/:batch", std::move(batch),
                       std::string(Encoding::kSitosV1Batch));
  EXPECT_EQ(Access::Get(cache, "a")->As<std::int64_t>(), 4);

  transport->EmitOwned("sitos/session/s1/:batch", {std::byte{0xff}},
                       std::string(Encoding::kSitosV1Batch));
  EXPECT_FALSE(Access::Get(cache, "bad").has_value());
  EXPECT_EQ(Access::Get(cache, "a")->As<std::int64_t>(), 4);
}

TEST(ParamCacheTest, DetachClearsStateAndRejectsLateSamples) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.AttachBase().IsOk());
  transport->EmitOwned("sitos/base/key", Payload(ParamValue(1)));
  ASSERT_TRUE(Access::Get(cache, "key").has_value());
  cache.Detach();
  EXPECT_FALSE(Access::IsAttached(cache));
  transport->EmitOwned("sitos/base/key", Payload(ParamValue(2)));
  EXPECT_FALSE(Access::Get(cache, "key").has_value());
  cache.Detach();
}

TEST(ParamCacheTest, AttachBaseUsesSubscriberFirstAndDeleteErases) {
  auto transport = std::make_shared<FakeTransport>();
  transport->replies["sitos/base/**"] = {
      {"sitos/base/key", Payload(ParamValue(1)), Encoding{std::string(Encoding::kSitosV1)}}};
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.AttachBase().IsOk());
  ASSERT_EQ(transport->calls.size(), 2U);
  EXPECT_TRUE(transport->calls[0].starts_with("declare:sitos/base/**"));
  EXPECT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 1);
  transport->EmitOwned("sitos/base/key", {}, "ignored", TransportSample::Kind::Delete);
  EXPECT_FALSE(Access::Get(cache, "key").has_value());
}

TEST(ParamCacheTest, UnknownEncodingUsesBytesAndMalformedKnownValueIsIgnored) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.AttachBase().IsOk());
  transport->EmitOwned("sitos/base/raw", {std::byte{1}, std::byte{2}}, "application/octet-stream");
  auto raw = Access::Get(cache, "raw");
  ASSERT_TRUE(raw.has_value());
  ASSERT_TRUE(raw->As<std::vector<std::byte>>().has_value());
  EXPECT_EQ(raw->As<std::vector<std::byte>>()->size(), 2U);
  transport->EmitOwned("sitos/base/bad", {std::byte{0xff}}, std::string(Encoding::kSitosV1));
  EXPECT_FALSE(Access::Get(cache, "bad").has_value());
}

TEST(ParamCacheTest, FailedAttachPreservesTransportErrorAndCanRetry) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  const auto cause = std::make_error_code(std::errc::connection_reset);
  transport->get_result = Result<void>::Err(sitos::Status::Disconnected, "offline", cause);
  auto failed = cache.AttachBase();
  ASSERT_FALSE(failed.IsOk());
  EXPECT_EQ(failed.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(failed.Message(), "offline");
  EXPECT_EQ(failed.Error(), cause);
  EXPECT_FALSE(Access::IsAttached(cache));
  transport->get_result = Result<void>::Ok();
  ASSERT_TRUE(cache.AttachBase().IsOk());
}

}  // namespace
