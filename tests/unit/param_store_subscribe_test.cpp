// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

class CaptureSink final : public sitos::LogSink {
 public:
  explicit CaptureSink(bool throw_on_write = false) : throw_on_write_(throw_on_write) {}

  void Write(const sitos::LogRecord& record) override {
    if (throw_on_write_) throw std::runtime_error("log sink failed");
    std::lock_guard<std::mutex> lock(mutex_);
    levels.push_back(record.level);
    messages.emplace_back(record.message);
  }

  std::size_t Count(sitos::LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count(levels.begin(), levels.end(), level));
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return levels.size();
  }

 private:
  bool throw_on_write_;
  mutable std::mutex mutex_;
  std::vector<sitos::LogLevel> levels;
  std::vector<std::string> messages;
};

class FakeTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Get(std::string_view, const QueryResultSink&,
                          std::chrono::milliseconds) override {
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
                                          sitos::Encoding{
                                              std::string(sitos::Encoding::kSitosV1Batch)},
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

TEST(ParamStoreSubscribeTest, CapturesWarningsAndErrorsAndSupportsDisabledLogging) {
  auto sink = std::make_shared<CaptureSink>();
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int callbacks = 0;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    ++callbacks;
    throw std::runtime_error("callback failed");
  });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitRaw("sitos/base/malformed", {}, std::string(sitos::Encoding::kSitosV1));
  transport->EmitPut("sitos/base/unknown", sitos::ParamValue(true), "application/octet-stream");
  EXPECT_EQ(callbacks, 1);
  EXPECT_GE(sink->Count(sitos::LogLevel::kWarning), 2U);
  EXPECT_EQ(sink->Count(sitos::LogLevel::kError), 1U);

  auto disabled_transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig disabled_config;
  disabled_config.log_sink = nullptr;
  auto disabled_store_result = sitos::ParamStore::Open(disabled_transport, disabled_config);
  ASSERT_TRUE(disabled_store_result.IsOk());
  auto disabled_store = std::move(disabled_store_result).Value();
  auto disabled_subscription = disabled_store.Subscribe(
      "base", "", [](const sitos::ParamChange&) {});
  ASSERT_TRUE(disabled_subscription.IsOk());
  EXPECT_NO_THROW(disabled_transport->EmitRaw("sitos/base/malformed", {},
                                               std::string(sitos::Encoding::kSitosV1)));
}

TEST(ParamStoreSubscribeTest, ThrowingLogSinkIsContained) {
  auto sink = std::make_shared<CaptureSink>(true);
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  auto subscription = store.Subscribe("base", "", [](const sitos::ParamChange&) {
    throw std::runtime_error("callback failed");
  });
  ASSERT_TRUE(subscription.IsOk());
  EXPECT_NO_THROW(transport->EmitRaw("sitos/base/malformed", {},
                                     std::string(sitos::Encoding::kSitosV1)));
  EXPECT_NO_THROW(transport->EmitPut("sitos/base/value", sitos::ParamValue(true)));
}

TEST(ParamStoreSubscribeTest, SubscriptionSurvivesParamStoreMoveAndDestruction) {
  auto transport = std::make_shared<FakeTransport>();
  int callbacks = 0;
  std::optional<sitos::ParamSubscription> subscription;
  {
    auto store_result = sitos::ParamStore::Open(transport);
    ASSERT_TRUE(store_result.IsOk());
    auto store = std::move(store_result).Value();
    auto result = store.Subscribe("base", "", [&](const sitos::ParamChange&) { ++callbacks; });
    ASSERT_TRUE(result.IsOk());
    subscription.emplace(std::move(result).Value());
    auto moved_store = std::move(store);
    EXPECT_FALSE(store.Put("base", "moved", sitos::ParamValue(true)).IsOk());
  }
  ASSERT_TRUE(subscription.has_value());
  transport->EmitPut("sitos/base/value", sitos::ParamValue(true));
  EXPECT_EQ(callbacks, 1);
  subscription->Close();
}

TEST(ParamStoreSubscribeTest, ConcurrentRepeatedCloseIsSafe) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  auto subscription = store.Subscribe("base", "", [](const sitos::ParamChange&) {});
  ASSERT_TRUE(subscription.IsOk());
  std::vector<std::thread> closers;
  for (int i = 0; i < 4; ++i) {
    closers.emplace_back([&] { subscription.Value().Close(); });
  }
  for (auto& closer : closers) closer.join();
  subscription.Value().Close();
  EXPECT_NO_THROW(transport->EmitPut("sitos/base/after", sitos::ParamValue(true)));
}

TEST(ParamStoreSubscribeTest, RejectsMalformedBatchInputsWithoutCallbacks) {
  auto sink = std::make_shared<CaptureSink>();
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int callbacks = 0;
  auto subscription = store.Subscribe(
      "base", "", [&](const sitos::ParamChange&) { ++callbacks; });
  ASSERT_TRUE(subscription.IsOk());
  auto valid_entries = std::vector<sitos::BatchEntry>{{"valid", sitos::ParamValue(true)}};
  auto malformed = sitos::EncodeBatch(valid_entries);
  malformed.pop_back();
  transport->Emit(sitos::TransportSample{
      "sitos/base/:batch", malformed,
      sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)}, std::nullopt,
      sitos::TransportSample::Kind::Put});
  auto invalid_entries =
      std::vector<sitos::BatchEntry>{{"invalid*key", sitos::ParamValue(true)}};
  const auto invalid_batch = sitos::EncodeBatch(invalid_entries);
  transport->Emit(sitos::TransportSample{
      "sitos/base/:batch", invalid_batch,
      sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)}, std::nullopt,
      sitos::TransportSample::Kind::Put});
  const auto wrong_encoding_batch = sitos::EncodeBatch(valid_entries);
  transport->Emit(sitos::TransportSample{
      "sitos/base/:batch", wrong_encoding_batch, sitos::Encoding{"application/octet-stream"},
      std::nullopt, sitos::TransportSample::Kind::Put});
  transport->EmitDelete("sitos/base/:batch");
  EXPECT_EQ(callbacks, 0);
  EXPECT_EQ(sink->Count(sitos::LogLevel::kWarning), 4U);
}

TEST(ParamStoreSubscribeTest, RejectsMalformedOrdinaryPayloadsWithoutCallbacks) {
  auto sink = std::make_shared<CaptureSink>();
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int callbacks = 0;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) { ++callbacks; });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitRaw("sitos/base/value", {}, std::string(sitos::Encoding::kSitosV1));
  transport->EmitRaw("sitos/base/value", sitos::ParamValue(true).Encode(),
                     std::string(sitos::Encoding::kSitosV1Batch));
  EXPECT_EQ(callbacks, 0);
  EXPECT_EQ(sink->Count(sitos::LogLevel::kWarning), 2U);
}

TEST(ParamStoreSubscribeTest, RawPrefixBoundaryAndTrailingSlashArePreserved) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::vector<std::string> keys;
  auto subscription = store.Subscribe("base", "foo/", [&](const sitos::ParamChange& change) {
    keys.push_back(change.key);
  });
  ASSERT_TRUE(subscription.IsOk());
  transport->EmitPut("sitos/base/foo", sitos::ParamValue(true));
  transport->EmitPut("sitos/base/foo/bar", sitos::ParamValue(true));
  transport->EmitPut("sitos/base/foobar", sitos::ParamValue(true));
  ASSERT_EQ(keys.size(), 1U);
  EXPECT_EQ(keys.front(), "foo/bar");
}

TEST(ParamStoreSubscribeTest, CloseDrainsPendingBatchAndBlocksPostCloseDiagnostics) {
  auto sink = std::make_shared<CaptureSink>();
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  std::vector<std::string> keys;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange& change) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      keys.push_back(change.key);
      if (keys.size() == 1U) entered = true;
    }
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      first = keys.size() == 1U;
    }
    condition.notify_all();
    if (first) {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return release; });
    }
  });
  ASSERT_TRUE(subscription.IsOk());
  std::vector<sitos::BatchEntry> entries{
      {"one", sitos::ParamValue(true)}, {"two", sitos::ParamValue(false)},
      {"three", sitos::ParamValue(true)}};
  std::thread emitter([&] {
    transport->Emit(sitos::TransportSample{"sitos/base/:batch", sitos::EncodeBatch(entries),
                                            sitos::Encoding{
                                                std::string(sitos::Encoding::kSitosV1Batch)},
                                            std::nullopt, sitos::TransportSample::Kind::Put});
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
  }
  std::thread closer([&] { subscription.Value().Close(); });
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  emitter.join();
  closer.join();
  ASSERT_EQ(keys.size(), entries.size());
  EXPECT_EQ(keys[0], "one");
  EXPECT_EQ(keys[1], "two");
  EXPECT_EQ(keys[2], "three");
  const auto log_count = sink->Size();
  transport->EmitRaw("sitos/base/after", {}, std::string(sitos::Encoding::kSitosV1));
  EXPECT_EQ(sink->Size(), log_count);
}

TEST(ParamStoreSubscribeTest, ConcurrentNativeCallbacksAreSerialized) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  constexpr int kEmitters = 4;
  std::mutex mutex;
  std::condition_variable condition;
  int started = 0;
  int callbacks = 0;
  int active = 0;
  int max_active = 0;
  bool release = false;
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++active;
      ++callbacks;
      max_active = std::max(max_active, active);
    }
    condition.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return release; });
    --active;
    lock.unlock();
    condition.notify_all();
  });
  ASSERT_TRUE(subscription.IsOk());
  std::vector<std::thread> emitters;
  for (int i = 0; i < kEmitters; ++i) {
    emitters.emplace_back([&, i] {
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++started;
      }
      condition.notify_all();
      transport->EmitPut("sitos/base/value" + std::to_string(i), sitos::ParamValue(true));
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return started == kEmitters; }));
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  for (auto& emitter : emitters) emitter.join();
  EXPECT_EQ(callbacks, kEmitters);
  EXPECT_EQ(max_active, 1);
}

TEST(ParamStoreSubscribeTest, ValidationPrecedesDeclaration) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  auto empty_callback = store.Subscribe("base", "", {});
  EXPECT_FALSE(empty_callback.IsOk());
  EXPECT_EQ(empty_callback.StatusCode(), sitos::Status::InvalidArgument);
  auto snapshot = store.Subscribe("snap/session", "", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(snapshot.IsOk());
  EXPECT_EQ(snapshot.StatusCode(), sitos::Status::InvalidArgument);
  auto malformed_scope = store.Subscribe("session", "", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(malformed_scope.IsOk());
  EXPECT_EQ(malformed_scope.StatusCode(), sitos::Status::InvalidKey);
  auto malformed_session = store.Subscribe("session/bad$id", "",
                                            [](const sitos::ParamChange&) {});
  EXPECT_FALSE(malformed_session.IsOk());
  EXPECT_EQ(malformed_session.StatusCode(), sitos::Status::InvalidKey);
  auto malformed = store.Subscribe("base", "bad*prefix", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(malformed.IsOk());
  EXPECT_EQ(malformed.StatusCode(), sitos::Status::InvalidKey);
  auto moved_store = std::move(store);
  auto moved_from = store.Subscribe("base", "", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(moved_from.IsOk());
  EXPECT_EQ(moved_from.StatusCode(), sitos::Status::InvalidArgument);
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
