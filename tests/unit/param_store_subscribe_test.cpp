// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"
#include "param_store_test_access.hpp"

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

class BlockingSink final : public sitos::LogSink {
 public:
  void Write(const sitos::LogRecord&) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
      ++writes_;
    }
    condition_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return release_; });
  }

  bool WaitForEntry() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    condition_.notify_all();
  }

  int Writes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writes_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool release_ = false;
  int writes_ = 0;
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
    std::vector<std::function<void(const sitos::TransportSample&)>> callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex);
      callbacks = subscribers;
    }
    ASSERT_FALSE(callbacks.empty());
    for (const auto& callback : callbacks) callback(sample);
  }

  void EmitToSelector(std::string_view fragment, const sitos::TransportSample& sample) {
    std::function<void(const sitos::TransportSample&)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (std::size_t index = 0; index < subscribers.size(); ++index) {
        if (subscriber_selectors[index].find(fragment) != std::string::npos) {
          callback = subscribers[index];
          break;
        }
      }
    }
    ASSERT_TRUE(callback);
    callback(sample);
  }

  void EmitTo(std::size_t index, const sitos::TransportSample& sample) {
    std::function<void(const sitos::TransportSample&)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ASSERT_LT(index, subscribers.size());
      callback = subscribers[index];
    }
    callback(sample);
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

  ~FakeTransport() {
    if (declaration_thread.joinable()) declaration_thread.join();
  }

  void NotifyDeclarationProgress() { declaration_condition.notify_all(); }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    ++declaration_count;
    declared_keyexpr = std::string(keyexpr);
    {
      std::lock_guard<std::mutex> lock(mutex);
      subscribers.push_back(callback);
      subscriber_selectors.emplace_back(keyexpr);
      subscriber = callback;
    }
    if (sample_during_declaration.has_value()) {
      if (async_declaration_sample) {
        declaration_thread = std::thread(
            [callback, this] { callback(*sample_during_declaration); });
        if (declaration_callback_admitted) {
          std::unique_lock<std::mutex> lock(declaration_mutex);
          declaration_condition.wait(lock, [&] { return declaration_callback_admitted(); });
        }
      } else {
        callback(*sample_during_declaration);
      }
    }
    if (declaration_error.has_value()) return std::move(*declaration_error);
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
  std::vector<std::function<void(const sitos::TransportSample&)>> subscribers;
  std::vector<std::string> subscriber_selectors;
  std::mutex mutex;
  std::optional<sitos::TransportSample> sample_during_declaration;
  std::optional<sitos::Result<sitos::Subscription>> declaration_error;
  bool async_declaration_sample = false;
  std::thread declaration_thread;
  std::function<bool()> declaration_callback_admitted;
  std::mutex declaration_mutex;
  std::condition_variable declaration_condition;
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
  bool close_admission_observed = false;
  std::atomic<bool> close_returned = false;
  sitos::param_store_test_access::ParamStoreTestAccess::SetLifecycleHooks(store, {}, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      close_admission_observed = true;
    }
    condition.notify_all();
  });
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
  bool callback_entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    callback_entered_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
  }
  std::thread closer([&] {
    subscription.Value().Close();
    close_returned.store(true, std::memory_order_release);
  });
  bool close_admission_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    close_admission_in_time = condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return close_admission_observed; });
  }
  EXPECT_TRUE(callback_entered_in_time);
  EXPECT_TRUE(close_admission_in_time);
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

TEST(ParamStoreSubscribeTest, CloseDeliversAdmittedSampleBeforeReturning) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  std::mutex mutex;
  std::condition_variable condition;
  bool native_entered = false;
  bool release_native = false;
  bool close_admission = false;
  bool callback_delivered = false;
  bool close_returned = false;
  sitos::param_store_test_access::ParamStoreTestAccess::SetNativeEntryHook(store, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      native_entered = true;
    }
    condition.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return release_native; });
  });
  sitos::param_store_test_access::ParamStoreTestAccess::SetLifecycleHooks(store, {}, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      close_admission = true;
    }
    condition.notify_all();
  });
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange&) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      callback_delivered = true;
    }
    condition.notify_all();
  });
  ASSERT_TRUE(subscription.IsOk());

  std::thread emitter([&] {
    transport->EmitPut("sitos/base/admitted", sitos::ParamValue(true));
  });
  bool native_entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    native_entered_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return native_entered; });
  }
  std::thread closer([&] {
    subscription.Value().Close();
    {
      std::lock_guard<std::mutex> lock(mutex);
      close_returned = true;
    }
    condition.notify_all();
  });
  bool close_admission_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    close_admission_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return close_admission; });
  }
  bool returned_before_release = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    returned_before_release = close_returned;
  }
  EXPECT_TRUE(native_entered_in_time);
  EXPECT_TRUE(close_admission_in_time);
  EXPECT_FALSE(returned_before_release);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_native = true;
  }
  condition.notify_all();
  emitter.join();
  closer.join();
  EXPECT_TRUE(callback_delivered);
  EXPECT_TRUE(close_returned);
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
  int disabled_callbacks = 0;
  auto disabled_subscription = disabled_store.Subscribe(
      "base", "", [&](const sitos::ParamChange&) { ++disabled_callbacks; });
  ASSERT_TRUE(disabled_subscription.IsOk());
  EXPECT_NO_THROW(disabled_transport->EmitRaw("sitos/base/malformed", {},
                                               std::string(sitos::Encoding::kSitosV1)));
  disabled_transport->EmitPut("sitos/base/valid", sitos::ParamValue(true));
  EXPECT_EQ(disabled_callbacks, 1);
}

TEST(ParamStoreSubscribeTest, CloseWaitsForBlockedLogSink) {
  auto sink = std::make_shared<BlockingSink>();
  auto transport = std::make_shared<FakeTransport>();
  sitos::ClientConfig config;
  config.log_sink = sink;
  auto store_result = sitos::ParamStore::Open(transport, config);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex close_mutex;
  std::condition_variable close_condition;
  bool close_admission = false;
  sitos::param_store_test_access::ParamStoreTestAccess::SetLifecycleHooks(store, {}, [&] {
    {
      std::lock_guard<std::mutex> lock(close_mutex);
      close_admission = true;
    }
    close_condition.notify_all();
  });
  auto subscription = store.Subscribe("base", "", [](const sitos::ParamChange&) {});
  ASSERT_TRUE(subscription.IsOk());

  std::thread emitter([&] {
    transport->EmitRaw("sitos/base/malformed", {}, std::string(sitos::Encoding::kSitosV1));
  });
  const bool sink_entered = sink->WaitForEntry();
  if (!sink_entered) {
    sink->Release();
    emitter.join();
    ADD_FAILURE() << "log sink did not enter";
    return;
  }
  std::atomic<bool> close_returned = false;
  std::thread closer([&] {
    subscription.Value().Close();
    close_returned.store(true, std::memory_order_release);
  });
  bool close_admission_in_time = false;
  {
    std::unique_lock<std::mutex> lock(close_mutex);
    close_admission_in_time = close_condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return close_admission; });
  }
  EXPECT_TRUE(close_admission_in_time);
  EXPECT_FALSE(close_returned.load(std::memory_order_acquire));
  sink->Release();
  emitter.join();
  closer.join();
  EXPECT_TRUE(close_returned.load(std::memory_order_acquire));
  const int writes = sink->Writes();
  transport->EmitRaw("sitos/base/after-close", {}, std::string(sitos::Encoding::kSitosV1));
  EXPECT_EQ(sink->Writes(), writes);
}

TEST(ParamStoreSubscribeTest, ThrowingLogSinkIsContained) {
  auto sink = std::make_shared<CaptureSink>(true);
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
  EXPECT_NO_THROW(transport->EmitPut("sitos/base/first", sitos::ParamValue(true)));
  EXPECT_NO_THROW(transport->EmitPut("sitos/base/second", sitos::ParamValue(false)));
  EXPECT_EQ(callbacks, 2);
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

TEST(ParamStoreSubscribeTest, ParamSubscriptionOwnsAndReleasesDependencies) {
  auto transport = std::make_shared<FakeTransport>();
  auto sink = std::make_shared<CaptureSink>();
  const std::weak_ptr<FakeTransport> weak_transport = transport;
  const std::weak_ptr<CaptureSink> weak_sink = sink;
  std::optional<sitos::ParamSubscription> subscription;
  {
    sitos::ClientConfig config;
    config.log_sink = sink;
    auto store_result = sitos::ParamStore::Open(transport, config);
    ASSERT_TRUE(store_result.IsOk());
    auto store = std::move(store_result).Value();
    auto result = store.Subscribe("base", "", [](const sitos::ParamChange&) {});
    ASSERT_TRUE(result.IsOk());
    subscription.emplace(std::move(result).Value());
  }
  transport.reset();
  sink.reset();
  EXPECT_FALSE(weak_transport.expired());
  EXPECT_FALSE(weak_sink.expired());
  auto held_transport = weak_transport.lock();
  ASSERT_TRUE(held_transport);
  held_transport->EmitRaw("sitos/base/malformed", {}, std::string(sitos::Encoding::kSitosV1));
  held_transport.reset();
  subscription->Close();
  EXPECT_TRUE(weak_transport.expired());
  EXPECT_TRUE(weak_sink.expired());
}

TEST(ParamStoreSubscribeTest, ConcurrentRepeatedCloseIsSafe) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex mutex;
  std::condition_variable condition;
  bool native_entered = false;
  bool release_native = false;
  bool close_admission = false;
  int started = 0;
  int returned = 0;
  sitos::param_store_test_access::ParamStoreTestAccess::SetNativeEntryHook(store, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      native_entered = true;
    }
    condition.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return release_native; });
  });
  sitos::param_store_test_access::ParamStoreTestAccess::SetLifecycleHooks(store, {}, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      close_admission = true;
    }
    condition.notify_all();
  });
  auto subscription = store.Subscribe("base", "", [](const sitos::ParamChange&) {});
  ASSERT_TRUE(subscription.IsOk());
  std::thread emitter([&] {
    transport->EmitPut("sitos/base/admitted", sitos::ParamValue(true));
  });
  bool native_entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    native_entered_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return native_entered; });
  }
  std::vector<std::thread> closers;
  for (int i = 0; i < 4; ++i) {
    closers.emplace_back([&] {
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++started;
      }
      condition.notify_all();
      subscription.Value().Close();
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++returned;
      }
      condition.notify_all();
    });
  }
  bool all_started_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    all_started_in_time = condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return started == 4; });
  }
  bool close_admission_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    close_admission_in_time = condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return close_admission; });
  }
  bool all_blocked = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    all_blocked = returned == 0;
    release_native = true;
  }
  condition.notify_all();
  emitter.join();
  for (auto& closer : closers) closer.join();
  EXPECT_TRUE(native_entered_in_time);
  EXPECT_TRUE(all_started_in_time);
  EXPECT_TRUE(close_admission_in_time);
  EXPECT_TRUE(all_blocked);
  EXPECT_EQ(returned, 4);
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
  auto invalid_entries = std::vector<sitos::BatchEntry>{{"valid-before-invalid", sitos::ParamValue(true)},
                                                          {"invalid*key", sitos::ParamValue(true)}};
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
  bool entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    entered_in_time = condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
  }
  bool close_started = false;
  std::atomic<bool> close_returned = false;
  std::mutex close_mutex;
  std::condition_variable close_condition;
  std::thread closer([&] {
    {
      std::lock_guard<std::mutex> lock(close_mutex);
      close_started = true;
    }
    close_condition.notify_all();
    subscription.Value().Close();
    close_returned.store(true, std::memory_order_release);
  });
  bool close_started_in_time = false;
  {
    std::unique_lock<std::mutex> lock(close_mutex);
    close_started_in_time = close_condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return close_started; });
  }
  EXPECT_TRUE(entered_in_time);
  EXPECT_TRUE(close_started_in_time);
  EXPECT_FALSE(close_returned.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  emitter.join();
  closer.join();
  EXPECT_TRUE(close_returned.load(std::memory_order_acquire));
  std::vector<std::string> observed_keys;
  {
    std::lock_guard<std::mutex> lock(mutex);
    observed_keys = keys;
  }
  ASSERT_EQ(observed_keys.size(), entries.size());
  EXPECT_EQ(observed_keys[0], "one");
  EXPECT_EQ(observed_keys[1], "two");
  EXPECT_EQ(observed_keys[2], "three");
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
  int admitted = 0;
  int finished = 0;
  int callbacks = 0;
  int active = 0;
  int max_active = 0;
  bool release = false;
  sitos::param_store_test_access::ParamStoreTestAccess::SetNativeEntryHook(store, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++admitted;
    }
    condition.notify_all();
  });
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
      transport->EmitPut("sitos/base/value" + std::to_string(i), sitos::ParamValue(true));
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++finished;
      }
      condition.notify_all();
    });
  }
  bool all_admitted_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    all_admitted_in_time = condition.wait_for(lock, std::chrono::seconds(3),
                                              [&] { return admitted == kEmitters; });
  }
  EXPECT_TRUE(all_admitted_in_time);
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(finished, 0);
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  for (auto& emitter : emitters) emitter.join();
  EXPECT_EQ(finished, kEmitters);
  EXPECT_EQ(callbacks, kEmitters);
  EXPECT_EQ(max_active, 1);
}

TEST(ParamStoreSubscribeTest, ConcurrentSubscribeHasIndependentQueues) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex mutex;
  std::condition_variable condition;
  int ready = 0;
  bool start = false;
  bool first_entered = false;
  bool release_first = false;
  bool second_delivered = false;
  std::optional<sitos::Result<sitos::ParamSubscription>> first;
  std::optional<sitos::Result<sitos::ParamSubscription>> second;
  std::thread first_thread([&] {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      condition.notify_all();
      condition.wait(lock, [&] { return start; });
    }
    first.emplace(store.Subscribe("base", "first", [&](const sitos::ParamChange&) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        first_entered = true;
      }
      condition.notify_all();
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return release_first; });
    }));
  });
  std::thread second_thread([&] {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      condition.notify_all();
      condition.wait(lock, [&] { return start; });
    }
    second.emplace(store.Subscribe("base", "second", [&](const sitos::ParamChange&) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        second_delivered = true;
      }
      condition.notify_all();
    }));
  });
  bool ready_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready_in_time = condition.wait_for(lock, std::chrono::seconds(3), [&] { return ready == 2; });
    start = true;
  }
  EXPECT_TRUE(ready_in_time);
  condition.notify_all();
  first_thread.join();
  second_thread.join();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(first->IsOk());
  ASSERT_TRUE(second->IsOk());

  auto first_sample = MakePutSample();
  first_sample.key = "sitos/base/first/value";
  auto second_sample = MakePutSample();
  second_sample.key = "sitos/base/second/value";
  std::size_t first_index = 0;
  std::thread first_emitter([&] { transport->EmitTo(0, first_sample); });
  bool first_entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    first_entered_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return first_entered; });
  }
  if (!first_entered_in_time) {
    first_emitter.join();
    first_index = 1;
    first_emitter = std::thread([&] { transport->EmitTo(1, first_sample); });
    {
      std::unique_lock<std::mutex> lock(mutex);
      first_entered_in_time =
          condition.wait_for(lock, std::chrono::seconds(3), [&] { return first_entered; });
    }
  }
  EXPECT_TRUE(first_entered_in_time);
  if (!first_entered_in_time) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      release_first = true;
    }
    condition.notify_all();
    first_emitter.join();
    return;
  }
  const std::size_t second_index = first_index == 0 ? 1 : 0;
  std::thread second_emitter([&] { transport->EmitTo(second_index, second_sample); });
  bool second_delivered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    second_delivered_in_time =
        condition.wait_for(lock, std::chrono::seconds(3), [&] { return second_delivered; });
  }
  EXPECT_TRUE(second_delivered_in_time);
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  condition.notify_all();
  first_emitter.join();
  second_emitter.join();
}

TEST(ParamStoreSubscribeTest, BatchDoesNotInterleaveConcurrentOrdinarySample) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  std::mutex mutex;
  std::condition_variable condition;
  bool first_entered = false;
  bool ordinary_admitted = false;
  bool ordinary_finished = false;
  bool release = false;
  bool release_ordinary = false;
  int native_admitted = 0;
  std::vector<std::string> keys;
  sitos::param_store_test_access::ParamStoreTestAccess::SetNativeEntryHook(store, [&] {
    std::unique_lock<std::mutex> lock(mutex);
    ++native_admitted;
    if (native_admitted == 2) {
      ordinary_admitted = true;
      condition.notify_all();
      condition.wait(lock, [&] { return release_ordinary; });
    }
  });
  auto subscription = store.Subscribe("base", "", [&](const sitos::ParamChange& change) {
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      keys.push_back(change.key);
      first = keys.size() == 1U;
      if (first) first_entered = true;
    }
    condition.notify_all();
    if (first) {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return release; });
    }
  });
  ASSERT_TRUE(subscription.IsOk());
  std::vector<sitos::BatchEntry> entries{
      {"batch/one", sitos::ParamValue(true)}, {"batch/two", sitos::ParamValue(false)}};
  std::thread batch_thread([&] {
    transport->Emit(sitos::TransportSample{"sitos/base/:batch", sitos::EncodeBatch(entries),
                                            sitos::Encoding{
                                                std::string(sitos::Encoding::kSitosV1Batch)},
                                            std::nullopt, sitos::TransportSample::Kind::Put});
  });
  bool entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    entered_in_time = condition.wait_for(lock, std::chrono::seconds(3),
                                         [&] { return first_entered; });
  }
  std::thread ordinary_thread([&] {
    transport->EmitPut("sitos/base/ordinary", sitos::ParamValue(true));
    {
      std::lock_guard<std::mutex> lock(mutex);
      ordinary_finished = true;
    }
    condition.notify_all();
  });
  bool ordinary_admitted_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ordinary_admitted_in_time = condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return ordinary_admitted; });
  }
  EXPECT_TRUE(entered_in_time);
  EXPECT_TRUE(ordinary_admitted_in_time);
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_FALSE(ordinary_finished);
    release = true;
    release_ordinary = true;
  }
  condition.notify_all();
  batch_thread.join();
  ordinary_thread.join();
  std::vector<std::string> observed;
  {
    std::lock_guard<std::mutex> lock(mutex);
    observed = keys;
  }
  ASSERT_EQ(observed.size(), 3U);
  EXPECT_EQ(observed[0], "batch/one");
  EXPECT_EQ(observed[1], "batch/two");
  EXPECT_EQ(observed[2], "ordinary");
}

TEST(ParamStoreSubscribeTest, MoveAssignmentClosesDestinationAndDestructorStopsDelivery) {
  auto transport = std::make_shared<FakeTransport>();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  int old_callbacks = 0;
  int source_callbacks = 0;
  auto old = store.Subscribe("base", "old", [&](const sitos::ParamChange&) { ++old_callbacks; });
  auto source = store.Subscribe("base", "source", [&](const sitos::ParamChange&) {
    ++source_callbacks;
  });
  ASSERT_TRUE(old.IsOk());
  ASSERT_TRUE(source.IsOk());
  old.Value() = std::move(source).Value();
  transport->EmitPut("sitos/base/old/value", sitos::ParamValue(true));
  transport->EmitPut("sitos/base/source/value", sitos::ParamValue(true));
  EXPECT_EQ(old_callbacks, 0);
  EXPECT_EQ(source_callbacks, 1);

  int destroyed_callbacks = 0;
  {
    auto temporary = store.Subscribe("base", "destroyed", [&](const sitos::ParamChange&) {
      ++destroyed_callbacks;
    });
    ASSERT_TRUE(temporary.IsOk());
  }
  transport->EmitPut("sitos/base/destroyed/value", sitos::ParamValue(true));
  EXPECT_EQ(destroyed_callbacks, 0);
}

TEST(ParamStoreSubscribeTest, TransportAndLogSinkSurviveParamStoreDestruction) {
  auto transport = std::make_shared<FakeTransport>();
  auto sink = std::make_shared<CaptureSink>();
  std::optional<sitos::ParamSubscription> subscription;
  int callbacks = 0;
  {
    sitos::ClientConfig config;
    config.log_sink = sink;
    auto store_result = sitos::ParamStore::Open(transport, config);
    ASSERT_TRUE(store_result.IsOk());
    auto store = std::move(store_result).Value();
    auto result = store.Subscribe("base", "", [&](const sitos::ParamChange&) { ++callbacks; });
    ASSERT_TRUE(result.IsOk());
    subscription.emplace(std::move(result).Value());
  }
  transport->EmitPut("sitos/base/value", sitos::ParamValue(true));
  transport->EmitRaw("sitos/base/malformed", {}, std::string(sitos::Encoding::kSitosV1));
  EXPECT_EQ(callbacks, 1);
  EXPECT_EQ(sink->Count(sitos::LogLevel::kWarning), 1U);
  subscription->Close();
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
  auto empty_and_malformed = store.Subscribe("bad scope", "bad*prefix", {});
  EXPECT_FALSE(empty_and_malformed.IsOk());
  EXPECT_EQ(empty_and_malformed.StatusCode(), sitos::Status::InvalidArgument);
  auto malformed_scope_and_prefix =
      store.Subscribe("session", "bad*prefix", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(malformed_scope_and_prefix.IsOk());
  EXPECT_EQ(malformed_scope_and_prefix.StatusCode(), sitos::Status::InvalidKey);
  auto snapshot_and_prefix =
      store.Subscribe("snap/session", "bad*prefix", [](const sitos::ParamChange&) {});
  EXPECT_FALSE(snapshot_and_prefix.IsOk());
  EXPECT_EQ(snapshot_and_prefix.StatusCode(), sitos::Status::InvalidArgument);
  auto moved_store = std::move(store);
  auto moved_and_malformed = store.Subscribe("bad scope", "bad*prefix", {});
  EXPECT_FALSE(moved_and_malformed.IsOk());
  EXPECT_EQ(moved_and_malformed.StatusCode(), sitos::Status::InvalidArgument);
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

TEST(ParamStoreSubscribeTest, DeclarationFailureWaitsForAdmittedCallbackCopy) {
  auto transport = std::make_shared<FakeTransport>();
  transport->sample_during_declaration = MakePutSample();
  transport->async_declaration_sample = true;
  transport->declaration_error = sitos::Result<sitos::Subscription>::Err(
      sitos::Status::Disconnected, "declaration failed", std::make_error_code(std::errc::io_error));
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  std::mutex mutex;
  std::condition_variable condition;
  std::atomic<bool> hook_entered = false;
  bool fail_staging_entered = false;
  bool release_hook = false;
  bool subscribe_finished = false;
  int callback_count = 0;
  sitos::param_store_test_access::ParamStoreTestAccess::SetLifecycleHooks(store, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      fail_staging_entered = true;
    }
    condition.notify_all();
  }, {});
  transport->declaration_callback_admitted =
      [&] { return hook_entered.load(std::memory_order_acquire); };
  sitos::param_store_test_access::ParamStoreTestAccess::SetNativeEntryHook(store, [&] {
    hook_entered.store(true, std::memory_order_release);
    transport->NotifyDeclarationProgress();
    {
      std::lock_guard<std::mutex> lock(mutex);
    }
    condition.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return release_hook; });
  });

  std::optional<sitos::Result<sitos::ParamSubscription>> result;
  std::thread subscriber_thread([&] {
    result.emplace(store.Subscribe("base", "", [&](const sitos::ParamChange&) {
      ++callback_count;
    }));
    {
      std::lock_guard<std::mutex> lock(mutex);
      subscribe_finished = true;
    }
    condition.notify_all();
  });
  bool hook_entered_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    hook_entered_in_time = condition.wait_for(lock, std::chrono::seconds(3), [&] {
      return hook_entered.load(std::memory_order_acquire);
    });
  }
  bool fail_staging_in_time = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    fail_staging_in_time = condition.wait_for(
        lock, std::chrono::seconds(3), [&] { return fail_staging_entered; });
  }
  EXPECT_TRUE(hook_entered_in_time);
  EXPECT_TRUE(fail_staging_in_time);
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_FALSE(subscribe_finished);
    release_hook = true;
  }
  condition.notify_all();
  subscriber_thread.join();

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->IsOk());
  EXPECT_EQ(result->StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(result->Message(), "declaration failed");
  EXPECT_EQ(result->Error(), std::make_error_code(std::errc::io_error));
  EXPECT_EQ(callback_count, 0);
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
