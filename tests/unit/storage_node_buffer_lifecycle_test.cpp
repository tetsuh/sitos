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
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

class LifecycleEngine final : public StorageEngine {
 public:
  enum class SnapshotMode { Normal, Null, Throw };

  explicit LifecycleEngine(std::shared_ptr<bool> destroyed = nullptr)
      : destroyed_(std::move(destroyed)) {}
  ~LifecycleEngine() override {
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
  void PutSample(std::string key) {
    std::vector<std::byte> value{std::byte{1}};
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
  LifecycleTransport transport;
  auto base = std::make_shared<LifecycleEngine>();
  auto node = MakeNode(transport, base);
  auto no_factory = node->CreateSession("missing", {.durable_buffers = true});
  EXPECT_EQ(no_factory.StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(no_factory.Error(), std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(no_factory.Message(), "durable buffer engine factory is required");
  EXPECT_TRUE(node->CreateSession("missing").IsOk());

  int calls = 0;
  LifecycleTransport null_transport;
  auto null_node =
      MakeNode(null_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
        EXPECT_EQ(sid, "null");
        ++calls;
        return Result<std::unique_ptr<StorageEngine>>::Ok(nullptr);
      });
  auto null_result = null_node->CreateSession("null", {.durable_buffers = true});
  EXPECT_EQ(null_result.StatusCode(), Status::Error);
  EXPECT_EQ(null_result.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(null_result.Message(), "durable buffer engine factory returned null");
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(null_node->CreateSession("null").IsOk());

  LifecycleTransport throw_transport;
  auto throw_node = MakeNode(throw_transport, std::make_shared<LifecycleEngine>(),
                             [&](std::string_view) -> Result<std::unique_ptr<StorageEngine>> {
                               ++calls;
                               throw std::runtime_error("hidden detail");
                             });
  auto throw_result = throw_node->CreateSession("throw", {.durable_buffers = true});
  EXPECT_EQ(throw_result.StatusCode(), Status::Error);
  EXPECT_EQ(throw_result.Message(), "durable buffer engine factory threw an exception");
  EXPECT_EQ(calls, 2);

  LifecycleTransport err_transport;
  const auto cause = std::make_error_code(std::errc::permission_denied);
  auto err_node =
      MakeNode(err_transport, std::make_shared<LifecycleEngine>(), [&](std::string_view) {
        return Result<std::unique_ptr<StorageEngine>>::Err(Status::Disconnected,
                                                           "factory-owned failure", cause);
      });
  auto err_result = err_node->CreateSession("err", {.durable_buffers = true});
  EXPECT_EQ(err_result.StatusCode(), Status::Disconnected);
  EXPECT_EQ(err_result.Error(), cause);
  EXPECT_EQ(err_result.Message(), "factory-owned failure");
  EXPECT_TRUE(err_node->CreateSession("err").IsOk());

  LifecycleTransport setup_transport;
  auto setup_base = std::make_shared<LifecycleEngine>();
  int setup_calls = 0;
  auto setup_node = MakeNode(setup_transport, setup_base, [&](std::string_view) {
    ++setup_calls;
    return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<LifecycleEngine>());
  });
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Null;
  auto snapshot_null = setup_node->CreateSession("snapshot-null", {.durable_buffers = true});
  EXPECT_EQ(snapshot_null.StatusCode(), Status::Error);
  EXPECT_EQ(snapshot_null.Error(), MakeErrorCode(Status::Error));
  EXPECT_EQ(snapshot_null.Message(), "base snapshot creation returned null");
  EXPECT_EQ(setup_calls, 0);
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Throw;
  auto snapshot_throw = setup_node->CreateSession("snapshot-throw", {.durable_buffers = true});
  EXPECT_EQ(snapshot_throw.StatusCode(), Status::Error);
  EXPECT_EQ(snapshot_throw.Message(), "base snapshot creation threw an exception");
  EXPECT_EQ(setup_calls, 0);
  setup_base->snapshot_mode = LifecycleEngine::SnapshotMode::Normal;

  EXPECT_EQ(setup_node->CreateSession("bad sid", {.durable_buffers = true}).StatusCode(),
            Status::InvalidArgument);
  EXPECT_EQ(setup_calls, 0);
  ASSERT_TRUE(setup_node->CreateSession("collision").IsOk());
  EXPECT_EQ(setup_node->CreateSession("collision", {.durable_buffers = true}).StatusCode(),
            Status::Error);
  EXPECT_EQ(setup_calls, 0);

  setup_node->Stop();
  for (auto result : {setup_node->CreateSession("stopped"),
                      setup_node->CreateSession("stopped", {.durable_buffers = true})}) {
    EXPECT_EQ(result.StatusCode(), Status::InvalidArgument);
    EXPECT_EQ(result.Error(), std::make_error_code(std::errc::invalid_argument));
    EXPECT_TRUE(result.Message().empty());
  }
  EXPECT_EQ(setup_calls, 0);
}

TEST(StorageNodeBufferLifecycleTest, FactoryAndStopLinearizeDeterministically) {
  LifecycleTransport transport;
  std::mutex mutex;
  std::condition_variable cv;
  bool factory_entered = false;
  bool release_factory = false;
  auto destroyed = std::make_shared<bool>(false);
  auto node = MakeNode(transport, std::make_shared<LifecycleEngine>(), [&](std::string_view sid) {
    EXPECT_EQ(sid, "s");
    {
      std::scoped_lock lock(mutex);
      factory_entered = true;
    }
    cv.notify_all();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return release_factory; });
    return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<LifecycleEngine>(destroyed));
  });
  std::atomic<bool> create_done = false;
  std::atomic<bool> stop_done = false;
  std::thread create([&] {
    EXPECT_TRUE(node->CreateSession("s", {.durable_buffers = true}).IsOk());
    create_done = true;
  });
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return factory_entered; });
  }
  std::thread stop([&] {
    node->Stop();
    stop_done = true;
  });
  std::this_thread::yield();
  EXPECT_FALSE(create_done.load());
  EXPECT_FALSE(stop_done.load());
  {
    std::scoped_lock lock(mutex);
    release_factory = true;
  }
  cv.notify_all();
  create.join();
  stop.join();
  EXPECT_TRUE(create_done);
  EXPECT_TRUE(stop_done);
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

  std::atomic<bool> close_started = false;
  std::atomic<bool> closed = false;
  std::thread close([&] {
    close_started = true;
    ASSERT_TRUE(node->CloseSession("s").IsOk());
    closed = true;
  });
  while (!close_started.load()) std::this_thread::yield();
  EXPECT_FALSE(closed.load());
  durable_raw->ReleaseAll();
  put.join();
  get.join();
  list.join();
  close.join();
  EXPECT_TRUE(closed.load());
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
