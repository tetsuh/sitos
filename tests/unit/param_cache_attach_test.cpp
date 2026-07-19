// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
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
#include "transport/declaration_handle_test_access.hpp"

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
    {
      std::lock_guard lock(mutex);
      const auto result_it = get_result_factories.find(std::string(keyexpr));
      if (result_it != get_result_factories.end()) return result_it->second();
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
    if (!declaration_result.IsOk()) return std::move(declaration_result);
    if (emit_during_declaration) {
      std::function<void(const TransportSample&)> callback_copy;
      {
        std::lock_guard lock(mutex);
        callback_copy = subscriber;
      }
      callback_copy(declaration_sample);
    }
    return Result<Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([this] {
          {
            std::lock_guard lock(mutex);
            ++reset_count;
          }
          if (reset_hook) reset_hook();
        }));
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
  std::unordered_map<std::string, std::function<Result<void>()>> get_result_factories;
  std::function<void()> get_hook;
  std::function<void(const TransportSample&)> subscriber;
  Result<Subscription> declaration_result = Result<Subscription>::Ok(Subscription{});
  Result<void> get_result = Result<void>::Ok();
  bool emit_during_declaration = false;
  std::vector<std::byte> declaration_payload;
  TransportSample declaration_sample{
      "sitos/session/s1/declaration", {}, Encoding{std::string(Encoding::kSitosV1)}, std::nullopt,
      TransportSample::Kind::Put};
  std::atomic<int> reset_count{0};
  std::function<void()> reset_hook;
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
  EXPECT_EQ(transport->calls[0], "declare:sitos/session/s1/**");
  EXPECT_EQ(transport->calls[1], "get:sitos/snap/s1/**");
  EXPECT_EQ(transport->calls[2], "get:sitos/session/s1/**");
  EXPECT_EQ(Access::Get(cache, "inherited")->As<std::int64_t>(), 3);
  EXPECT_EQ(Access::Get(cache, "overlaid")->As<std::int64_t>(), 2);
}

TEST(ParamCacheTest, DeclarationCallbackIsBufferedBeforeInitialGet) {
  auto transport = std::make_shared<FakeTransport>();
  const auto payload = Payload(ParamValue(std::int64_t{9}));
  transport->declaration_payload = payload;
  transport->declaration_sample = TransportSample{
      "sitos/session/s1/declaration", transport->declaration_payload,
      Encoding{std::string(Encoding::kSitosV1)}, std::nullopt, TransportSample::Kind::Put};
  transport->emit_during_declaration = true;
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_TRUE(Access::Get(cache, "declaration").has_value());
  EXPECT_EQ(Access::Get(cache, "declaration")->As<std::int64_t>(), 9);
}

TEST(ParamCacheTest, AttachRejectsInvalidSidAndAlreadyAttachedWithoutWireCalls) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  EXPECT_EQ(cache.Attach("bad sid").StatusCode(), sitos::Status::InvalidKey);
  EXPECT_TRUE(transport->calls.empty());
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  const auto call_count = transport->calls.size();
  EXPECT_EQ(cache.Attach("s1").StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport->calls.size(), call_count);
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

  auto malformed_batch = sitos::EncodeBatch(
      std::vector<BatchEntry>{{"prevalidated", ParamValue(5)}, {"malformed", ParamValue(6)}});
  ASSERT_FALSE(malformed_batch.empty());
  malformed_batch.pop_back();
  transport->EmitOwned("sitos/session/s1/:batch", std::move(malformed_batch),
                       std::string(Encoding::kSitosV1Batch));
  EXPECT_FALSE(Access::Get(cache, "prevalidated").has_value());
  EXPECT_FALSE(Access::Get(cache, "malformed").has_value());

  const std::vector<BatchEntry> invalid_key_entries = {
      {"valid_first", ParamValue(7)}, {"invalid key", ParamValue(8)}};
  auto invalid_key_batch = sitos::EncodeBatch(invalid_key_entries);
  transport->EmitOwned("sitos/session/s1/:batch", std::move(invalid_key_batch),
                       std::string(Encoding::kSitosV1Batch));
  EXPECT_FALSE(Access::Get(cache, "valid_first").has_value());

  transport->EmitOwned("sitos/session/s1/:batch", {std::byte{0xff}},
                       std::string(Encoding::kSitosV1Batch));
  EXPECT_FALSE(Access::Get(cache, "bad").has_value());
  EXPECT_EQ(Access::Get(cache, "a")->As<std::int64_t>(), 4);
}

TEST(ParamCacheTest, BatchAndOrdinarySamplesDoNotInterleave) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  std::mutex mutex;
  std::condition_variable condition;
  bool first_mutation = false;
  bool release = false;
  Access::SetMutationHook(cache, [&](std::size_t count) {
    if (count != 1) return;
    std::unique_lock lock(mutex);
    first_mutation = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  });
  const std::vector<BatchEntry> entries = {{"batch_a", ParamValue(1)},
                                            {"batch_b", ParamValue(2)}};
  auto batch = sitos::EncodeBatch(entries);
  std::thread batch_thread([&] {
    transport->EmitOwned("sitos/session/s1/:batch", std::move(batch),
                         std::string(Encoding::kSitosV1Batch));
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return first_mutation; }));
  }
  std::thread ordinary_thread([&] {
    transport->EmitOwned("sitos/session/s1/ordinary", Payload(ParamValue(3)));
  });
  EXPECT_FALSE(Access::Get(cache, "ordinary").has_value());
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  batch_thread.join();
  ordinary_thread.join();
  EXPECT_EQ(Access::Get(cache, "batch_a")->As<std::int64_t>(), 1);
  EXPECT_EQ(Access::Get(cache, "batch_b")->As<std::int64_t>(), 2);
  EXPECT_EQ(Access::Get(cache, "ordinary")->As<std::int64_t>(), 3);
}

TEST(ParamCacheTest, DetachWaitsForInFlightCallbackAndRejectsStaleCallback) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  bool detach_returned = false;
  Access::SetCallbackHook(cache, [&] {
    std::unique_lock lock(mutex);
    callback_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_callback; });
  });
  std::thread sample_thread([&] {
    transport->EmitOwned("sitos/session/s1/in_flight", Payload(ParamValue(1)));
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return callback_entered; }));
  }
  transport->reset_hook = [&] {
    std::lock_guard lock(mutex);
    condition.notify_all();
  };
  std::thread detach_thread([&] {
    cache.Detach();
    std::lock_guard lock(mutex);
    detach_returned = true;
    condition.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return transport->reset_count == 1;
    }));
    EXPECT_FALSE(detach_returned);
  }
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  condition.notify_all();
  sample_thread.join();
  detach_thread.join();
  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_FALSE(Access::Get(cache, "in_flight").has_value());
  transport->EmitOwned("sitos/session/s1/stale", Payload(ParamValue(2)));
  EXPECT_FALSE(Access::Get(cache, "stale").has_value());
  EXPECT_EQ(transport->reset_count, 1);
}

TEST(ParamCacheTest, ActiveMoveAssignmentResetsDestinationExactlyOnce) {
  auto first_transport = std::make_shared<FakeTransport>();
  auto second_transport = std::make_shared<FakeTransport>();
  auto first_result = ParamCache::Open(first_transport);
  auto second_result = ParamCache::Open(second_transport);
  ASSERT_TRUE(first_result.IsOk());
  ASSERT_TRUE(second_result.IsOk());
  auto first = std::move(first_result).Value();
  auto second = std::move(second_result).Value();
  ASSERT_TRUE(first.Attach("s1").IsOk());
  ASSERT_TRUE(second.Attach("s1").IsOk());
  second = std::move(first);
  EXPECT_EQ(second_transport->reset_count, 1);
  EXPECT_TRUE(Access::IsAttached(second));
  second.Detach();
  EXPECT_EQ(first_transport->reset_count, 1);
}

TEST(ParamCacheTest, AttachedMoveConstructionTransfersOwnership) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  auto moved = std::move(cache);
  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_TRUE(Access::IsAttached(moved));
  EXPECT_EQ(transport->reset_count, 0);
  moved.Detach();
  EXPECT_EQ(transport->reset_count, 1);
}

TEST(ParamCacheTest, SelfMovePreservesActiveAttachment) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  cache = std::move(cache);
  EXPECT_TRUE(Access::IsAttached(cache));
  EXPECT_EQ(transport->reset_count, 0);
  cache.Detach();
  EXPECT_EQ(transport->reset_count, 1);
}

TEST(ParamCacheTest, ActiveMoveAssignmentWaitsForInFlightDestinationCallback) {
  auto source_transport = std::make_shared<FakeTransport>();
  auto destination_transport = std::make_shared<FakeTransport>();
  auto source_result = ParamCache::Open(source_transport);
  auto destination_result = ParamCache::Open(destination_transport);
  ASSERT_TRUE(source_result.IsOk());
  ASSERT_TRUE(destination_result.IsOk());
  auto source = std::move(source_result).Value();
  auto destination = std::move(destination_result).Value();
  ASSERT_TRUE(source.Attach("s1").IsOk());
  ASSERT_TRUE(destination.Attach("s1").IsOk());

  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  bool assignment_returned = false;
  Access::SetCallbackHook(destination, [&] {
    std::unique_lock lock(mutex);
    callback_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_callback; });
  });
  destination_transport->reset_hook = [&] { condition.notify_all(); };
  std::thread sample_thread([&] {
    destination_transport->EmitOwned("sitos/session/s1/in_flight", Payload(ParamValue(1)));
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return callback_entered;
    }));
  }

  std::thread assignment_thread([&] {
    destination = std::move(source);
    std::lock_guard lock(mutex);
    assignment_returned = true;
    condition.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return destination_transport->reset_count == 1;
    }));
    EXPECT_FALSE(assignment_returned);
  }
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  condition.notify_all();
  sample_thread.join();
  assignment_thread.join();

  EXPECT_TRUE(Access::IsAttached(destination));
  EXPECT_FALSE(Access::IsAttached(source));
  destination.Detach();
  EXPECT_EQ(destination_transport->reset_count, 1);
  EXPECT_EQ(source_transport->reset_count, 1);
}

TEST(ParamCacheTest, DestructionWaitsForInFlightCallback) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  std::optional<ParamCache> cache(std::move(result).Value());
  ASSERT_TRUE(cache->Attach("s1").IsOk());

  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  bool destruction_returned = false;
  Access::SetCallbackHook(*cache, [&] {
    std::unique_lock lock(mutex);
    callback_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_callback; });
  });
  transport->reset_hook = [&] { condition.notify_all(); };
  std::thread sample_thread([&] {
    transport->EmitOwned("sitos/session/s1/in_flight", Payload(ParamValue(1)));
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return callback_entered;
    }));
  }

  std::thread destruction_thread([&] {
    cache.reset();
    std::lock_guard lock(mutex);
    destruction_returned = true;
    condition.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&] {
      return transport->reset_count == 1;
    }));
    EXPECT_FALSE(destruction_returned);
  }
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  condition.notify_all();
  sample_thread.join();
  destruction_thread.join();
  EXPECT_TRUE(destruction_returned);
  EXPECT_FALSE(cache.has_value());
}

TEST(ParamCacheTest, DetachClearsStateAndRejectsLateSamples) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  transport->EmitOwned("sitos/session/s1/key", Payload(ParamValue(1)));
  ASSERT_TRUE(Access::Get(cache, "key").has_value());
  cache.Detach();
  EXPECT_FALSE(Access::IsAttached(cache));
  transport->EmitOwned("sitos/session/s1/key", Payload(ParamValue(2)));
  EXPECT_FALSE(Access::Get(cache, "key").has_value());
  cache.Detach();
}

TEST(ParamCacheTest, SessionAttachUsesSubscriberFirstAndDeleteRestoresBaseline) {
  auto transport = std::make_shared<FakeTransport>();
  transport->replies["sitos/snap/s1/**"] = {
      {"sitos/snap/s1/key", Payload(ParamValue(1)), Encoding{std::string(Encoding::kSitosV1)}}};
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_EQ(transport->calls.size(), 3U);
  EXPECT_EQ(transport->calls[0], "declare:sitos/session/s1/**");
  EXPECT_EQ(transport->calls[1], "get:sitos/snap/s1/**");
  EXPECT_EQ(transport->calls[2], "get:sitos/session/s1/**");
  EXPECT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 1);
  transport->EmitOwned("sitos/session/s1/key", Payload(ParamValue(2)));
  EXPECT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 2);
  transport->EmitOwned("sitos/session/s1/key", {}, "ignored", TransportSample::Kind::Delete);
  EXPECT_EQ(Access::Get(cache, "key")->As<std::int64_t>(), 1);
}

TEST(ParamCacheTest, ForeignScopeSamplesAreIgnored) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->EmitOwned("sitos/base/base_value", Payload(ParamValue(1)));
  transport->EmitOwned("sitos/snap/s1/snapshot_value", Payload(ParamValue(2)));
  transport->EmitOwned("sitos/session/s2/foreign_value", Payload(ParamValue(3)));

  EXPECT_FALSE(Access::Get(cache, "base_value").has_value());
  EXPECT_FALSE(Access::Get(cache, "snapshot_value").has_value());
  EXPECT_FALSE(Access::Get(cache, "foreign_value").has_value());
}

TEST(ParamCacheTest, ForeignDeleteBatchAndMalformedSamplesDoNotMutate) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  transport->EmitOwned("sitos/session/s1/unchanged", Payload(ParamValue(7)));

  transport->EmitOwned("sitos/base/unchanged", {}, "ignored", TransportSample::Kind::Delete);
  const std::vector<BatchEntry> base_entries = {{"unchanged", ParamValue(8)}};
  auto base_batch = sitos::EncodeBatch(base_entries);
  transport->EmitOwned("sitos/base/:batch", std::move(base_batch),
                       std::string(Encoding::kSitosV1Batch));
  transport->EmitOwned("malformed-key", Payload(ParamValue(9)));

  ASSERT_TRUE(Access::Get(cache, "unchanged").has_value());
  EXPECT_EQ(Access::Get(cache, "unchanged")->As<std::int64_t>(), 7);
  EXPECT_EQ(Access::Size(cache), 1U);
}

TEST(ParamCacheTest, UnknownEncodingUsesBytesAndMalformedKnownValueIsIgnored) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  transport->EmitOwned("sitos/session/s1/raw", {std::byte{1}, std::byte{2}}, "application/octet-stream");
  auto raw = Access::Get(cache, "raw");
  ASSERT_TRUE(raw.has_value());
  ASSERT_TRUE(raw->As<std::vector<std::byte>>().has_value());
  EXPECT_EQ(raw->As<std::vector<std::byte>>()->size(), 2U);
  transport->EmitOwned("sitos/session/s1/bad", {std::byte{0xff}}, std::string(Encoding::kSitosV1));
  EXPECT_FALSE(Access::Get(cache, "bad").has_value());
}

TEST(ParamCacheTest, FailedAttachStagesRollbackPreservesErrorAndCanRetry) {
  const auto cause = std::make_error_code(std::errc::connection_reset);

  {
    auto transport = std::make_shared<FakeTransport>();
    auto result = ParamCache::Open(transport);
    ASSERT_TRUE(result.IsOk());
    auto cache = std::move(result).Value();
    transport->declaration_result = Result<Subscription>::Err(
        sitos::Status::Disconnected, "declaration offline", cause);

    auto failed = cache.Attach("s1");
    ASSERT_FALSE(failed.IsOk());
    EXPECT_EQ(failed.StatusCode(), sitos::Status::Disconnected);
    EXPECT_EQ(failed.Message(), "declaration offline");
    EXPECT_EQ(failed.Error(), cause);
    EXPECT_FALSE(Access::IsAttached(cache));
    EXPECT_EQ(transport->reset_count, 0);

    transport->declaration_result = Result<Subscription>::Ok(Subscription{});
    ASSERT_TRUE(cache.Attach("s1").IsOk());
    cache.Detach();
    EXPECT_EQ(transport->reset_count, 1);
  }

  {
    auto transport = std::make_shared<FakeTransport>();
    auto result = ParamCache::Open(transport);
    ASSERT_TRUE(result.IsOk());
    auto cache = std::move(result).Value();
    transport->get_result_factories["sitos/snap/s1/**"] = [cause, first = true]() mutable {
      if (first) {
        first = false;
        return Result<void>::Err(sitos::Status::Disconnected, "snapshot offline", cause);
      }
      return Result<void>::Ok();
    };

    auto failed = cache.Attach("s1");
    ASSERT_FALSE(failed.IsOk());
    EXPECT_EQ(failed.StatusCode(), sitos::Status::Disconnected);
    EXPECT_EQ(failed.Message(), "snapshot offline");
    EXPECT_EQ(failed.Error(), cause);
    EXPECT_FALSE(Access::IsAttached(cache));
    EXPECT_EQ(transport->reset_count, 1);
    EXPECT_EQ(Access::Size(cache), 0U);

    auto retry = cache.Attach("s1");
    ASSERT_TRUE(retry.IsOk()) << static_cast<int>(retry.StatusCode()) << ": " << retry.Message();
    cache.Detach();
    EXPECT_EQ(transport->reset_count, 2);
  }

  {
    auto transport = std::make_shared<FakeTransport>();
    transport->replies["sitos/snap/s1/**"] = {
        {"sitos/snap/s1/partial", Payload(ParamValue(1)),
         Encoding{std::string(Encoding::kSitosV1)}}};
    auto result = ParamCache::Open(transport);
    ASSERT_TRUE(result.IsOk());
    auto cache = std::move(result).Value();
    transport->get_result_factories["sitos/session/s1/**"] = [cause, first = true]() mutable {
      if (first) {
        first = false;
        return Result<void>::Err(sitos::Status::Error, "overlay invalid", cause);
      }
      return Result<void>::Ok();
    };

    auto failed = cache.Attach("s1");
    ASSERT_FALSE(failed.IsOk());
    EXPECT_EQ(failed.StatusCode(), sitos::Status::Error);
    EXPECT_EQ(failed.Message(), "overlay invalid");
    EXPECT_EQ(failed.Error(), cause);
    EXPECT_FALSE(Access::IsAttached(cache));
    EXPECT_FALSE(Access::Get(cache, "partial").has_value());
    EXPECT_EQ(transport->reset_count, 1);

    transport->get_result_factories.clear();
    ASSERT_TRUE(cache.Attach("s1").IsOk());
    EXPECT_EQ(Access::Get(cache, "partial")->As<std::int64_t>(), 1);
    cache.Detach();
    EXPECT_EQ(transport->reset_count, 2);
  }
}

TEST(ParamCacheTest, FailedAttachPreservesTransportErrorAndCanRetry) {
  auto transport = std::make_shared<FakeTransport>();
  auto result = ParamCache::Open(transport);
  ASSERT_TRUE(result.IsOk());
  auto cache = std::move(result).Value();
  const auto cause = std::make_error_code(std::errc::connection_reset);
  transport->get_result = Result<void>::Err(sitos::Status::Disconnected, "offline", cause);
  auto failed = cache.Attach("s1");
  ASSERT_FALSE(failed.IsOk());
  EXPECT_EQ(transport->reset_count, 1);
  EXPECT_EQ(failed.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(failed.Message(), "offline");
  EXPECT_EQ(failed.Error(), cause);
  EXPECT_FALSE(Access::IsAttached(cache));
  transport->get_result = Result<void>::Ok();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  EXPECT_EQ(transport->reset_count, 1);
  cache.Detach();
  EXPECT_EQ(transport->reset_count, 2);
}

}  // namespace
