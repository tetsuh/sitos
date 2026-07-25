// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  void Emit(const sitos::TransportSample& sample) {
    ASSERT_TRUE(static_cast<bool>(subscriber));
    subscriber(sample);
  }

  void EmitPut(std::string key, const sitos::ParamValue& value,
               std::string encoding = std::string(sitos::Encoding::kSitosV1)) {
    auto payload = value.Encode();
    Emit(sitos::TransportSample{std::move(key), payload, sitos::Encoding{std::move(encoding)},
                                std::nullopt, sitos::TransportSample::Kind::Put});
  }

  void EmitRaw(std::string key, std::vector<std::byte> payload, std::string encoding) {
    Emit(sitos::TransportSample{std::move(key), payload, sitos::Encoding{std::move(encoding)},
                                std::nullopt, sitos::TransportSample::Kind::Put});
  }

  void EmitDelete(std::string key) {
    Emit(sitos::TransportSample{std::move(key), {}, {}, std::nullopt,
                                sitos::TransportSample::Kind::Delete});
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    ++declaration_count;
    declared_keyexpr = std::string(keyexpr);
    subscriber = std::move(callback);
    if (declaration_error.has_value()) return std::move(*declaration_error);
    if (sample_during_declaration.has_value()) subscriber(*sample_during_declaration);
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Err(
        std::make_error_code(std::errc::operation_not_supported));
  }

  std::string declared_keyexpr;
  int declaration_count = 0;
  std::function<void(const sitos::TransportSample&)> subscriber;
  std::optional<sitos::TransportSample> sample_during_declaration;
  std::optional<sitos::Result<sitos::Subscription>> declaration_error;
};

sitos::TransportSample MakePutSample() {
  static const auto payload = sitos::ParamValue(true).Encode();
  return sitos::TransportSample{"sitos/base/flag", payload,
                                sitos::Encoding{std::string(sitos::Encoding::kSitosV1)},
                                std::nullopt, sitos::TransportSample::Kind::Put};
}

TEST(ParamStoreSubscribeTest, SynchronousDeclarationSampleIsStagedAndDrained) {
  auto transport = std::make_shared<FakeTransport>();
  transport->sample_during_declaration = MakePutSample();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  std::vector<sitos::ParamChange> changes;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange& change) {
    changes.push_back(change);
  });

  ASSERT_TRUE(subscription.IsOk());
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().kind, sitos::ParamChangeKind::kPut);
  EXPECT_EQ(changes.front().key, "flag");
  ASSERT_TRUE(changes.front().value.has_value());
  EXPECT_EQ(changes.front().value->As<bool>(), true);
}

TEST(ParamStoreSubscribeTest, DeliversDeleteAndUnknownEncodingAsBytes) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::vector<sitos::ParamChange> changes;
  auto subscription = store.Subscribe("base", "foo", [&](const sitos::ParamChange& change) {
    changes.push_back(change);
  });
  ASSERT_TRUE(subscription.IsOk());

  transport->EmitPut("sitos/base/foobar", sitos::ParamValue(true), "application/octet-stream");
  transport->EmitDelete("sitos/base/foo");
  ASSERT_EQ(changes.size(), 2U);
  EXPECT_EQ(changes[0].key, "foobar");
  ASSERT_TRUE(changes[0].value.has_value());
  ASSERT_TRUE(changes[0].value->As<std::vector<std::byte>>().has_value());
  EXPECT_EQ(changes[1].kind, sitos::ParamChangeKind::kDelete);
  EXPECT_EQ(changes[1].key, "foo");
  EXPECT_FALSE(changes[1].value.has_value());
}

TEST(ParamStoreSubscribeTest, BatchPreservesOrderAndDuplicates) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::vector<sitos::ParamChange> changes;
  auto subscription = store.Subscribe("base", "foo", [&](const sitos::ParamChange& change) {
    changes.push_back(change);
  });
  ASSERT_TRUE(subscription.IsOk());

  std::vector<sitos::BatchEntry> entries;
  entries.push_back({"foo/one", sitos::ParamValue(std::int64_t{1})});
  entries.push_back({"foobar", sitos::ParamValue(std::int64_t{2})});
  entries.push_back({"foo/one", sitos::ParamValue(std::int64_t{3})});
  auto payload = sitos::EncodeBatch(entries);
  transport->Emit(sitos::TransportSample{"sitos/base/:batch", payload,
                                          sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)},
                                          std::nullopt, sitos::TransportSample::Kind::Put});

  ASSERT_EQ(changes.size(), 3U);
  EXPECT_EQ(changes[0].key, "foo/one");
  EXPECT_EQ(changes[1].key, "foobar");
  EXPECT_EQ(changes[2].key, "foo/one");
  EXPECT_EQ(changes[0].value->As<std::int64_t>(), 1);
  EXPECT_EQ(changes[2].value->As<std::int64_t>(), 3);
}

TEST(ParamStoreSubscribeTest, CloseStopsDelivery) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int callback_count = 0;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    ++callback_count;
  });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitPut("sitos/base/value", sitos::ParamValue(true));
  ASSERT_EQ(callback_count, 1);
  subscription.Value().Close();
  subscription.Value().Close();
  transport->EmitPut("sitos/base/value", sitos::ParamValue(false));
  EXPECT_EQ(callback_count, 1);
}

TEST(ParamStoreSubscribeTest, CloseWaitsForBlockedCallback) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  bool close_started = false;
  std::atomic<bool> close_returned = false;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      entered = true;
    }
    condition.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return release; });
  });
  ASSERT_TRUE(subscription.IsOk());

  std::thread emitter([&] { transport->EmitPut("sitos/base/value", sitos::ParamValue(true)); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
  }
  std::thread closer([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      close_started = true;
    }
    condition.notify_all();
    subscription.Value().Close();
    close_returned.store(true, std::memory_order_release);
  });
  bool closer_started_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    closer_started_in_time = condition.wait_for(lock, std::chrono::seconds(3),
                                                [&] { return close_started; });
  }
  EXPECT_TRUE(closer_started_in_time);
  EXPECT_FALSE(close_returned.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  emitter.join();
  closer.join();
  EXPECT_TRUE(close_returned.load(std::memory_order_acquire));
}

TEST(ParamStoreSubscribeTest, ReentrantEmissionIsQueuedAfterCurrentCallback) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::vector<std::string> keys;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange& change) {
    keys.push_back(change.key);
    if (keys.size() == 1U) {
      transport->EmitPut("sitos/base/reentrant", sitos::ParamValue(true));
    }
  });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitPut("sitos/base/first", sitos::ParamValue(true));
  ASSERT_EQ(keys.size(), 2U);
  EXPECT_EQ(keys[0], "first");
  EXPECT_EQ(keys[1], "reentrant");
}

TEST(ParamStoreSubscribeTest, ValidationPrecedesDeclaration) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  auto snapshot = store.Subscribe("snap/session", "", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(snapshot.IsOk());
  EXPECT_EQ(snapshot.StatusCode(), sitos::Status::InvalidArgument);
  auto malformed = store.Subscribe("base", "bad*prefix", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(malformed.IsOk());
  EXPECT_EQ(malformed.StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(transport->declaration_count, 0);
}

TEST(ParamStoreSubscribeTest, CallbackExceptionsDoNotStopSubscription) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int callback_count = 0;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    ++callback_count;
    throw std::runtime_error("callback failed");
  });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitPut("sitos/base/one", sitos::ParamValue(true));
  transport->EmitPut("sitos/base/two", sitos::ParamValue(false));
  EXPECT_EQ(callback_count, 2);
}

TEST(ParamStoreSubscribeTest, DeclarationFailureDiscardsSynchronousSample) {
  auto transport = std::make_shared<FakeTransport>();
  transport->sample_during_declaration = MakePutSample();
  transport->declaration_error = sitos::Result<sitos::Subscription>::Err(
      sitos::Status::Disconnected, "declaration failed", std::make_error_code(std::errc::io_error));
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  int callback_count = 0;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    ++callback_count;
  });

  ASSERT_FALSE(subscription.IsOk());
  EXPECT_EQ(subscription.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(subscription.Message(), "declaration failed");
  EXPECT_EQ(subscription.Error(), std::make_error_code(std::errc::io_error));
  EXPECT_EQ(callback_count, 0);
}

}  // namespace
