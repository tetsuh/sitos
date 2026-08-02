// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sitos/storage_node.hpp"
#include "storage_node_test_access.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

class LifecycleEngine final : public StorageEngine {
 public:
  enum class SnapshotMode { Normal, Null, Throw };

  explicit LifecycleEngine(std::shared_ptr<bool> destroyed = nullptr)
      : destroyed_(std::move(destroyed)) {}
  ~LifecycleEngine() override {
    std::scoped_lock lock(release_mutex_);
    if (destroyed_) *destroyed_ = true;
  }

  bool Put(std::string_view key, Bytes value) override {
    WaitUntilReleased(block_put, put_entered);
    std::scoped_lock lock(mutex_);
    values_[std::string(key)] = {value.begin(), value.end()};
    return true;
  }
  bool Delete(std::string_view key) override {
    std::scoped_lock lock(mutex_);
    values_.erase(std::string(key));
    return true;
  }
  bool Get(std::string_view key, const EntrySink& sink) const override {
    WaitUntilReleased(block_get, get_entered);
    std::unique_lock lock(mutex_);
    auto it = values_.find(std::string(key));
    if (it == values_.end()) return false;
    auto copied_key = it->first;
    auto copied_value = it->second;
    lock.unlock();
    return sink(copied_key, copied_value);
  }
  bool List(std::string_view prefix, const EntrySink& sink) const override {
    WaitUntilReleased(block_list, list_entered);
    std::unique_lock lock(mutex_);
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    for (const auto& [key, value] : values_) {
      if (key.starts_with(prefix)) entries.emplace_back(key, value);
    }
    lock.unlock();
    for (const auto& [key, value] : entries) {
      if (!sink(key, value)) return false;
    }
    return true;
  }
  std::shared_ptr<const StorageReader> TakeSnapshot() const override {
    if (snapshot_mode == SnapshotMode::Throw) throw std::runtime_error("snapshot failure");
    if (snapshot_mode == SnapshotMode::Null) return nullptr;
    return StorageEngine::TakeSnapshot();
  }

  void WaitFor(int& count) {
    std::unique_lock lock(gate_mutex_);
    cv.wait(lock, [&] { return count > 0; });
  }
  void BlockGetAndList() {
    {
      std::scoped_lock lock(gate_mutex_);
      block_get = true;
      block_list = true;
    }
  }
  void ReleaseAll() {
    std::scoped_lock release_lock(release_mutex_);
    {
      std::scoped_lock lock(gate_mutex_);
      block_put = false;
      block_get = false;
      block_list = false;
    }
    cv.notify_all();
  }

  mutable std::mutex mutex_;
  mutable std::mutex gate_mutex_;
  mutable std::mutex release_mutex_;
  mutable std::condition_variable cv;
  mutable std::map<std::string, std::vector<std::byte>> values_;
  mutable int put_entered = 0;
  mutable int get_entered = 0;
  mutable int list_entered = 0;
  bool block_put = false;
  mutable bool block_get = false;
  mutable bool block_list = false;
  mutable SnapshotMode snapshot_mode = SnapshotMode::Normal;
  std::shared_ptr<bool> destroyed_;

 private:
  void WaitUntilReleased(bool& blocked, int& entered) const {
    std::unique_lock lock(gate_mutex_);
    if (!blocked) return;
    ++entered;
    cv.notify_all();
    cv.wait(lock, [&] { return !blocked; });
  }
};

class LifecycleTransport final : public Transport {
 public:
  Result<void> Put(std::string_view, std::span<const std::byte>, Encoding, PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }
  Result<void> Delete(std::string_view, PutOptions) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }
  Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }
  Result<Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const TransportSample&)> callback) override {
    subscriber = std::move(callback);
    return Result<Subscription>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }
  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)> callback) override {
    queryable = std::move(callback);
    return Result<Queryable>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }
  void PutSample(std::string key, std::vector<std::byte> value = {std::byte{1}}) {
    subscriber(TransportSample{std::move(key), value, Encoding{"zenoh/bytes"}, std::nullopt,
                               TransportSample::Kind::Put});
  }
  int Query(std::string key) {
    int replies = 0;
    auto query =
        TransportQuery::ForTesting([&](std::string_view, std::span<const std::byte>, Encoding) {
          ++replies;
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(key);
    queryable(query);
    return replies;
  }
  std::function<void(const TransportSample&)> subscriber;
  std::function<void(TransportQuery&)> queryable;
};

std::unique_ptr<StorageNode> MakeNode(LifecycleTransport& transport,
                                      std::shared_ptr<LifecycleEngine> base,
                                      DurableBufferEngineFactory factory = {}) {
  auto node = std::make_unique<StorageNode>(transport);
  EXPECT_TRUE(node->Start(std::move(base), transport,
                          {.prefix = "sitos",
                           .log_sink = nullptr,
                           .durable_buffer_engine_factory = std::move(factory)}));
  return node;
}

TEST(StorageNodeBufferLifecycleTest, FactoryFailureTaxonomyAndRollback) {
  LifecycleTransport no_factory_transport;
  auto no_factory_node = MakeNode(no_factory_transport, std::make_shared<LifecycleEngine>());
  auto no_factory = no_factory_node->CreateSession("missing", {.durable_buffers = true});
  EXPECT_EQ(no_factory.StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(no_factory.Error(), std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(no_factory.Message(), "durable buffer engine factory is required");
  EXPECT_TRUE(no_factory_node->CreateSession("missing").IsOk());

  int null_calls = 0;
  bool return_null = true;
  std::string null_sid;
  auto null_destroyed = std::make_shared<bool>(false);
  LifecycleTransport null_transport;
  auto null_node =
      MakeNode(null_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
        ++null_calls;
        null_sid = std::string(sid);
        if (return_null) {
          return Result<std::unique_ptr<StorageEngine>>::Ok(nullptr);
        }
        return Result<std::unique_ptr<StorageEngine>>::Ok(
            std::make_unique<LifecycleEngine>(null_destroyed));
      });
  auto null_result = null_node->CreateSession("null", {.durable_buffers = true});
  EXPECT_EQ(null_result.StatusCode(), Status::Error);
  EXPECT_EQ(null_result.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(null_result.Message(), "durable buffer engine factory returned null");
  EXPECT_EQ(null_sid, "null");
  return_null = false;
  EXPECT_TRUE(null_node->CreateSession("null", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(null_calls, 2);
  EXPECT_TRUE(null_node->CloseSession("null").IsOk());
  EXPECT_TRUE(*null_destroyed);

  int throw_calls = 0;
  bool throw_factory = true;
  std::string throw_sid;
  auto throw_destroyed = std::make_shared<bool>(false);
  LifecycleTransport throw_transport;
  auto throw_node =
      MakeNode(throw_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
        ++throw_calls;
        throw_sid = std::string(sid);
        if (throw_factory) throw std::runtime_error("hidden detail");
        return Result<std::unique_ptr<StorageEngine>>::Ok(
            std::make_unique<LifecycleEngine>(throw_destroyed));
      });
  auto throw_result = throw_node->CreateSession("throw", {.durable_buffers = true});
  EXPECT_EQ(throw_result.StatusCode(), Status::Error);
  EXPECT_EQ(throw_result.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(throw_result.Message(), "durable buffer engine factory threw an exception");
  EXPECT_EQ(throw_sid, "throw");
  throw_factory = false;
  EXPECT_TRUE(throw_node->CreateSession("throw", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(throw_calls, 2);
  EXPECT_TRUE(throw_node->CloseSession("throw").IsOk());
  EXPECT_TRUE(*throw_destroyed);

  LifecycleTransport err_transport;
  const auto cause = std::make_error_code(std::errc::permission_denied);
  int err_calls = 0;
  bool err_fail = true;
  std::string err_sid;
  auto err_destroyed = std::make_shared<bool>(false);
  LifecycleEngine* err_engine = nullptr;
  auto err_node =
      MakeNode(err_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
        ++err_calls;
        err_sid = std::string(sid);
        if (err_fail) {
          return Result<std::unique_ptr<StorageEngine>>::Err(Status::Disconnected,
                                                             "factory-owned failure", cause);
        }
        auto engine = std::make_unique<LifecycleEngine>(err_destroyed);
        err_engine = engine.get();
        return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(engine));
      });
  auto err_result = err_node->CreateSession("err", {.durable_buffers = true});
  EXPECT_EQ(err_result.StatusCode(), Status::Disconnected);
  EXPECT_EQ(err_result.Error(), cause);
  EXPECT_EQ(err_result.Message(), "factory-owned failure");
  EXPECT_EQ(err_calls, 1);
  EXPECT_EQ(err_sid, "err");
  err_fail = false;
  ASSERT_TRUE(err_node->CreateSession("err", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(err_calls, 2);
  err_transport.PutSample("sitos/buffers/err/durable/a", {std::byte{1}});
  err_transport.PutSample("sitos/buffers/err/durable/b", {std::byte{2}});
  ASSERT_NE(err_engine, nullptr);
  EXPECT_EQ(err_engine->values_.at("a"), (std::vector<std::byte>{std::byte{1}}));
  EXPECT_EQ(err_engine->values_.at("b"), (std::vector<std::byte>{std::byte{2}}));
  EXPECT_EQ(err_calls, 2);
  EXPECT_EQ(err_sid, "err");
  ASSERT_TRUE(err_node->CloseSession("err").IsOk());
  EXPECT_TRUE(*err_destroyed);

  LifecycleTransport setup_transport;
  auto setup_base = std::make_shared<LifecycleEngine>();
  int setup_calls = 0;
  std::string setup_sid;
  auto setup_destroyed = std::make_shared<bool>(false);
  auto setup_node = MakeNode(setup_transport, setup_base, [&](std::string_view sid) {
    ++setup_calls;
    setup_sid = std::string(sid);
    return Result<std::unique_ptr<StorageEngine>>::Ok(
        std::make_unique<LifecycleEngine>(setup_destroyed));
  });
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Null;
  auto snapshot_null = setup_node->CreateSession("snapshot-null", {.durable_buffers = true});
  EXPECT_EQ(snapshot_null.StatusCode(), Status::Error);
  EXPECT_EQ(snapshot_null.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(snapshot_null.Message(), "base snapshot creation returned null");
  EXPECT_EQ(setup_calls, 0);
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Normal;
  EXPECT_TRUE(setup_node->CreateSession("snapshot-null", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(setup_calls, 1);
  EXPECT_EQ(setup_sid, "snapshot-null");
  EXPECT_TRUE(setup_node->CloseSession("snapshot-null").IsOk());
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Throw;
  auto snapshot_throw = setup_node->CreateSession("snapshot-throw", {.durable_buffers = true});
  EXPECT_EQ(snapshot_throw.StatusCode(), Status::Error);
  EXPECT_EQ(snapshot_throw.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(snapshot_throw.Message(), "base snapshot creation threw an exception");
  EXPECT_EQ(setup_calls, 1);
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Normal;
  EXPECT_TRUE(setup_node->CreateSession("snapshot-throw", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(setup_calls, 2);

  EXPECT_EQ(setup_node->CreateSession("bad sid", {.durable_buffers = true}).StatusCode(),
            Status::InvalidArgument);
  EXPECT_EQ(setup_calls, 2);
  ASSERT_TRUE(setup_node->CreateSession("collision").IsOk());
  auto active_collision = setup_node->CreateSession("collision", {.durable_buffers = true});
  EXPECT_EQ(active_collision.StatusCode(), Status::Error);
  EXPECT_EQ(active_collision.Error(), std::make_error_code(std::errc::file_exists));
  EXPECT_EQ(setup_calls, 2);

  LifecycleTransport phase_transport;
  bool release_factory = false;
  std::mutex phase_mutex;
  std::condition_variable phase_cv;
  bool factory_entered = false;
  int phase_calls = 0;
  auto phase_node =
      MakeNode(phase_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
        ++phase_calls;
        {
          std::scoped_lock lock(phase_mutex);
          factory_entered = true;
        }
        phase_cv.notify_all();
        std::unique_lock lock(phase_mutex);
        phase_cv.wait(lock, [&] { return release_factory; });
        return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<LifecycleEngine>());
      });
  std::optional<Result<void>> creating_result;
  std::thread creating(
      [&] { creating_result = phase_node->CreateSession("phase", {.durable_buffers = true}); });
  {
    std::unique_lock lock(phase_mutex);
    phase_cv.wait(lock, [&] { return factory_entered; });
  }
  EXPECT_EQ(phase_transport.Query("sitos/buffers/phase/durable/**"), 0);
  auto creating_collision = phase_node->CreateSession("phase", {.durable_buffers = true});
  EXPECT_EQ(creating_collision.StatusCode(), Status::Error);
  EXPECT_EQ(creating_collision.Error(), std::make_error_code(std::errc::operation_in_progress));
  EXPECT_EQ(phase_calls, 1);
  {
    std::scoped_lock lock(phase_mutex);
    release_factory = true;
  }
  phase_cv.notify_all();
  creating.join();
  ASSERT_TRUE(creating_result.has_value());
  EXPECT_TRUE(creating_result->IsOk());
  EXPECT_TRUE(phase_node->CloseSession("phase").IsOk());

  LifecycleTransport closed_gate_transport;
  std::mutex closed_gate_mutex;
  std::condition_variable closed_gate_cv;
  bool state_captured = false;
  bool release_closed_gate_create = false;
  std::atomic<int> closed_gate_factory_calls = 0;
  auto closed_gate_node =
      MakeNode(closed_gate_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
        closed_gate_factory_calls.fetch_add(1, std::memory_order_relaxed);
        return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<LifecycleEngine>());
      });
  ASSERT_TRUE(storage_node_test_access::StorageNodeTestAccess::SetCreateSessionEntryObserver(
      *closed_gate_node, [&] {
        {
          std::scoped_lock lock(closed_gate_mutex);
          state_captured = true;
        }
        closed_gate_cv.notify_all();
        std::unique_lock lock(closed_gate_mutex);
        closed_gate_cv.wait(lock, [&] { return release_closed_gate_create; });
      }));
  std::optional<Result<void>> closed_gate_result;
  std::thread closed_gate_create([&] {
    closed_gate_result = closed_gate_node->CreateSession("closed-gate", {.durable_buffers = true});
  });
  {
    std::unique_lock lock(closed_gate_mutex);
    closed_gate_cv.wait(lock, [&] { return state_captured; });
  }
  closed_gate_node->Stop();
  {
    std::scoped_lock lock(closed_gate_mutex);
    release_closed_gate_create = true;
  }
  closed_gate_cv.notify_all();
  closed_gate_create.join();
  ASSERT_TRUE(closed_gate_result.has_value());
  EXPECT_EQ(closed_gate_result->StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(closed_gate_result->Error(), std::make_error_code(std::errc::invalid_argument));
  EXPECT_TRUE(closed_gate_result->Message().empty());
  EXPECT_EQ(closed_gate_factory_calls.load(std::memory_order_relaxed), 0);

  LifecycleTransport closing_transport;
  LifecycleEngine* closing_raw = nullptr;
  auto closing_destroyed = std::make_shared<bool>(false);
  int closing_calls = 0;
  auto closing_node =
      MakeNode(closing_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
        ++closing_calls;
        auto engine = std::make_unique<LifecycleEngine>(closing_destroyed);
        engine->block_put = true;
        closing_raw = engine.get();
        return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(engine));
      });
  ASSERT_TRUE(closing_node->CreateSession("closing", {.durable_buffers = true}).IsOk());
  std::thread blocked_put([&] { closing_transport.PutSample("sitos/buffers/closing/durable/k"); });
  closing_raw->WaitFor(closing_raw->put_entered);
  std::optional<Result<void>> close_result;
  std::atomic<bool> close_done = false;
  std::thread close([&] {
    close_result = closing_node->CloseSession("closing");
    close_done.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(
      storage_node_test_access::StorageNodeTestAccess::WaitForClosing(*closing_node, "closing"));
  EXPECT_EQ(closing_transport.Query("sitos/buffers/closing/durable/**"), 0);
  auto closing_collision = closing_node->CreateSession("closing", {.durable_buffers = true});
  EXPECT_EQ(closing_collision.StatusCode(), Status::Error);
  EXPECT_EQ(closing_collision.Error(), std::make_error_code(std::errc::operation_in_progress));
  EXPECT_FALSE(close_done.load(std::memory_order_acquire));
  closing_raw->ReleaseAll();
  blocked_put.join();
  close.join();
  ASSERT_TRUE(close_result.has_value());
  EXPECT_TRUE(close_result->IsOk());
  EXPECT_EQ(closing_calls, 1);
  EXPECT_TRUE(*closing_destroyed);

  auto success_destroyed = std::make_shared<bool>(false);
  std::string success_sid;
  int success_calls = 0;
  LifecycleTransport success_transport;
  auto success_node =
      MakeNode(success_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
        ++success_calls;
        success_sid = std::string(sid);
        return Result<std::unique_ptr<StorageEngine>>::Ok(
            std::make_unique<LifecycleEngine>(success_destroyed));
      });
  ASSERT_TRUE(success_node->CreateSession("success", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(success_calls, 1);
  EXPECT_EQ(success_sid, "success");
  ASSERT_TRUE(success_node->CloseSession("success").IsOk());
  EXPECT_TRUE(*success_destroyed);

  setup_node->Stop();
  for (auto result : {setup_node->CreateSession("stopped"),
                      setup_node->CreateSession("stopped", {.durable_buffers = true})}) {
    EXPECT_EQ(result.StatusCode(), Status::InvalidArgument);
    EXPECT_EQ(result.Error(), std::make_error_code(std::errc::invalid_argument));
    EXPECT_TRUE(result.Message().empty());
  }
  EXPECT_EQ(setup_calls, 2);
}

TEST(StorageNodeBufferLifecycleTest, FactoryAndStopLinearizeDeterministically) {
  LifecycleTransport transport;
  std::mutex mutex;
  std::condition_variable cv;
  bool factory_entered = false;
  bool release_factory = false;
  auto destroyed = std::make_shared<bool>(false);
  std::string factory_sid;
  auto node = MakeNode(transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
    factory_sid = std::string(sid);
    {
      std::scoped_lock lock(mutex);
      factory_entered = true;
    }
    cv.notify_all();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return release_factory; });
    return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<LifecycleEngine>(destroyed));
  });
  auto observer = storage_node_test_access::StorageNodeTestAccess::CaptureGateObserver(*node);
  ASSERT_TRUE(observer.has_value());
  std::optional<Result<void>> create_result;
  std::atomic<bool> stop_done = false;
  std::thread create([&] { create_result = node->CreateSession("s", {.durable_buffers = true}); });
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return factory_entered; });
  }
  std::thread stop([&] {
    node->Stop();
    stop_done = true;
  });
  ASSERT_TRUE(observer->WaitForClosed());
  EXPECT_FALSE(stop_done.load());
  EXPECT_EQ(node->CreateSession("late", {.durable_buffers = true}).StatusCode(),
            Status::InvalidArgument);
  {
    std::scoped_lock lock(mutex);
    release_factory = true;
  }
  cv.notify_all();
  create.join();
  stop.join();
  ASSERT_TRUE(create_result.has_value());
  EXPECT_TRUE(create_result->IsOk());
  EXPECT_EQ(factory_sid, "s");
  EXPECT_TRUE(stop_done.load());
  EXPECT_TRUE(*destroyed);
}

TEST(StorageNodeBufferLifecycleTest, CloseQuiescesDurableOperationsAndDestroysEngine) {
  LifecycleTransport transport;
  auto destroyed = std::make_shared<bool>(false);
  LifecycleEngine* durable_raw = nullptr;
  auto node = MakeNode(transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
    auto durable = std::make_unique<LifecycleEngine>(destroyed);
    durable->block_put = true;
    durable_raw = durable.get();
    return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(durable));
  });
  ASSERT_TRUE(node->CreateSession("s", {.durable_buffers = true}).IsOk());

  std::thread put([&] { transport.PutSample("sitos/buffers/s/durable/put"); });
  durable_raw->WaitFor(durable_raw->put_entered);
  durable_raw->BlockGetAndList();
  std::thread get([&] { transport.Query("sitos/buffers/s/durable/get"); });
  std::thread list([&] { transport.Query("sitos/buffers/s/durable/**"); });
  durable_raw->WaitFor(durable_raw->get_entered);
  durable_raw->WaitFor(durable_raw->list_entered);

  std::optional<Result<void>> close_result;
  std::atomic<bool> close_done = false;
  std::thread close([&] {
    close_result = node->CloseSession("s");
    close_done.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(storage_node_test_access::StorageNodeTestAccess::WaitForClosing(*node, "s"));
  EXPECT_FALSE(close_done.load(std::memory_order_acquire));
  durable_raw->ReleaseAll();
  put.join();
  get.join();
  list.join();
  close.join();
  ASSERT_TRUE(close_result.has_value());
  EXPECT_TRUE(close_result->IsOk());
  EXPECT_TRUE(*destroyed);
}

TEST(StorageNodeBufferLifecycleTest, SameSidRecreationUsesFreshEngine) {
  LifecycleTransport transport;
  int calls = 0;
  std::vector<std::shared_ptr<bool>> destroyed;
  auto node = MakeNode(transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
    ++calls;
    auto was_destroyed = std::make_shared<bool>(false);
    destroyed.push_back(was_destroyed);
    return Result<std::unique_ptr<StorageEngine>>::Ok(
        std::make_unique<LifecycleEngine>(was_destroyed));
  });
  ASSERT_TRUE(node->CreateSession("s", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k");
  ASSERT_TRUE(node->CloseSession("s").IsOk());
  ASSERT_TRUE(node->CreateSession("s", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(*destroyed[0]);
  EXPECT_FALSE(*destroyed[1]);
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/k"), 0);
}

}  // namespace
}  // namespace sitos
