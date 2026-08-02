// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"
#include "storage_node_test_access.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace {

class RocksDbTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    auto query = sitos::TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          if (sink(key, payload, std::move(encoding))) {
            return sitos::Result<void>::Ok();
          }
          return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
        });
    query.keyexpr = std::string(keyexpr);
    if (queryable_) {
      queryable_(query);
    }
    return sitos::Result<void>::Ok();
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    subscriber_ = std::move(callback);
    return sitos::Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)> callback) override {
    queryable_ = std::move(callback);
    return sitos::Result<sitos::Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

  sitos::Result<void> PutSample(std::string key, std::vector<std::byte> payload) {
    subscriber_(sitos::TransportSample{
        std::move(key), payload, {"zenoh/bytes"}, std::nullopt, sitos::TransportSample::Kind::Put});
    return sitos::Result<void>::Ok();
  }

 private:
  std::function<void(const sitos::TransportSample&)> subscriber_;
  std::function<void(sitos::TransportQuery&)> queryable_;
};

struct GateState {
  mutable std::mutex mutex;
  std::condition_variable condition;
  bool block_put = false;
  bool block_get = false;
  bool block_list = false;
  int put_entered = 0;
  int get_entered = 0;
  int list_entered = 0;
};

class BlockingRocksDbEngine final : public sitos::StorageEngine {
 public:
  BlockingRocksDbEngine(std::unique_ptr<sitos::RocksDBEngine> engine,
                        std::shared_ptr<GateState> gates, std::shared_ptr<bool> destroyed)
      : engine_(std::move(engine)), gates_(std::move(gates)), destroyed_(std::move(destroyed)) {}

  ~BlockingRocksDbEngine() override { *destroyed_ = true; }

  bool Put(std::string_view key, sitos::Bytes value) override {
    Wait(gates_->block_put, gates_->put_entered);
    return engine_->Put(key, value);
  }

  bool Delete(std::string_view key) override { return engine_->Delete(key); }

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    Wait(gates_->block_get, gates_->get_entered);
    return engine_->Get(key, sink);
  }

  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    Wait(gates_->block_list, gates_->list_entered);
    return engine_->List(prefix, sink);
  }

  std::shared_ptr<const sitos::StorageReader> TakeSnapshot() const override {
    return engine_->TakeSnapshot();
  }

 private:
  void Wait(bool& blocked, int& entered) const {
    std::unique_lock lock(gates_->mutex);
    if (!blocked) {
      return;
    }
    ++entered;
    gates_->condition.notify_all();
    gates_->condition.wait(lock, [&] { return !blocked; });
  }

  std::unique_ptr<sitos::RocksDBEngine> engine_;
  std::shared_ptr<GateState> gates_;
  std::shared_ptr<bool> destroyed_;
};

class ScopedRoot {
 public:
  explicit ScopedRoot(std::string_view label) {
    static std::atomic<unsigned long long> serial = 0;
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      const auto id = serial.fetch_add(1, std::memory_order_relaxed);
      const auto candidate = std::filesystem::temp_directory_path() /
                             ("sitos-issue56-" + std::string(label) + "-" +
                              std::to_string(timestamp) + "-" + std::to_string(id));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error != std::errc::file_exists) {
        throw std::system_error(error, "create temporary RocksDB directory");
      }
    }
    throw std::runtime_error("unable to allocate a unique temporary RocksDB directory");
  }

  ~ScopedRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScopedRoot(const ScopedRoot&) = delete;
  ScopedRoot& operator=(const ScopedRoot&) = delete;

  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ThreadScope {
 public:
  explicit ThreadScope(std::shared_ptr<GateState> gates) : gates_(std::move(gates)) {}

  ~ThreadScope() {
    Release();
    JoinAll();
  }

  void Add(std::thread& thread) { threads_.push_back(&thread); }

  void Release() {
    {
      std::scoped_lock lock(gates_->mutex);
      gates_->block_put = false;
      gates_->block_get = false;
      gates_->block_list = false;
    }
    gates_->condition.notify_all();
  }

  void JoinAll() {
    for (auto* thread : threads_) {
      if (thread->joinable()) {
        thread->join();
      }
    }
  }

 private:
  std::shared_ptr<GateState> gates_;
  std::vector<std::thread*> threads_;
};

bool WaitForMaterialization(std::chrono::milliseconds timeout,
                            const std::shared_ptr<GateState>& gates) {
  std::unique_lock lock(gates->mutex);
  return gates->condition.wait_for(lock, timeout, [&] {
    return gates->put_entered > 0 && gates->get_entered > 0 && gates->list_entered > 0;
  });
}

}  // namespace

TEST(RocksDBBufferLifecycleTest, CloseReleasesEngineBeforeReturn) {
  RocksDbTransport transport;
  ScopedRoot root("close");
  auto gates = std::make_shared<GateState>();
  auto destroyed = std::make_shared<bool>(false);
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      std::make_shared<sitos::InMemoryEngine>(),
      {.prefix = "sitos", .durable_buffer_engine_factory = [&](std::string_view sid) {
         auto opened = sitos::RocksDBEngine::Open((root.Path() / std::string(sid)).string());
         if (!opened.IsOk()) {
           return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
         }
         return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
             std::make_unique<BlockingRocksDbEngine>(std::move(opened).Value(), gates, destroyed));
       }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  {
    std::scoped_lock lock(gates->mutex);
    gates->block_put = true;
    gates->block_get = true;
    gates->block_list = true;
  }

  std::optional<sitos::Result<void>> put_result;
  std::optional<sitos::Result<void>> get_result;
  std::optional<sitos::Result<void>> list_result;
  ThreadScope threads(gates);
  std::thread put([&] {
    put_result = transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}});
  });
  threads.Add(put);
  std::thread get([&] {
    get_result = transport.Get(
        "sitos/buffers/session/durable/key", [](auto, auto, auto) { return true; },
        std::chrono::seconds(1));
  });
  threads.Add(get);
  std::thread list([&] {
    list_result = transport.Get(
        "sitos/buffers/session/durable/**", [](auto, auto, auto) { return true; },
        std::chrono::seconds(1));
  });
  threads.Add(list);

  if (!WaitForMaterialization(std::chrono::seconds(5), gates)) {
    threads.Release();
    threads.JoinAll();
    ADD_FAILURE() << "Put/Get/List did not all reach the real RocksDB barrier";
    return;
  }

  std::optional<sitos::Result<void>> close_result;
  std::atomic<bool> close_done = false;
  std::thread close([&] {
    close_result = node.CloseSession("session");
    close_done.store(true, std::memory_order_release);
  });
  threads.Add(close);
  const bool closing_seen =
      sitos::storage_node_test_access::StorageNodeTestAccess::WaitForClosing(node, "session");
  const bool close_done_while_blocked = close_done.load(std::memory_order_acquire);
  threads.Release();
  threads.JoinAll();

  EXPECT_TRUE(closing_seen);
  EXPECT_FALSE(close_done_while_blocked);
  ASSERT_TRUE(put_result.has_value());
  ASSERT_TRUE(get_result.has_value());
  ASSERT_TRUE(list_result.has_value());
  ASSERT_TRUE(close_result.has_value());
  EXPECT_TRUE(put_result->IsOk());
  EXPECT_TRUE(get_result->IsOk());
  EXPECT_TRUE(list_result->IsOk());
  EXPECT_TRUE(close_result->IsOk());
  EXPECT_TRUE(*destroyed);
  EXPECT_GT(std::filesystem::remove_all(root.Path()), 0u);
  EXPECT_FALSE(std::filesystem::exists(root.Path()));
}

TEST(RocksDBBufferLifecycleTest, SameSidRecreationUsesFreshEngine) {
  RocksDbTransport transport;
  ScopedRoot root("recreate");
  const auto first_root = root.Path() / "first";
  const auto second_root = root.Path() / "second";
  auto base = std::make_shared<sitos::InMemoryEngine>();
  std::vector<std::string> factory_sids;
  int factory_calls = 0;
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      base, {.prefix = "sitos", .durable_buffer_engine_factory = [&](std::string_view sid) {
               factory_sids.emplace_back(sid);
               ++factory_calls;
               const auto path = factory_calls == 1 ? first_root : second_root;
               auto opened = sitos::RocksDBEngine::Open((path / std::string(sid)).string());
               if (!opened.IsOk()) {
                 return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
               }
               return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                   std::make_unique<BlockingRocksDbEngine>(std::move(opened).Value(),
                                                           std::make_shared<GateState>(),
                                                           std::make_shared<bool>(false)));
             }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}}).IsOk());
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(first_root), 0u);
  EXPECT_FALSE(std::filesystem::exists(first_root));

  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  int replies = 0;
  ASSERT_TRUE(transport
                  .Get(
                      "sitos/buffers/session/durable/key",
                      [&](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                        ++replies;
                        return true;
                      },
                      std::chrono::milliseconds(500))
                  .IsOk());
  EXPECT_EQ(factory_calls, 2);
  EXPECT_EQ(factory_sids, (std::vector<std::string>{"session", "session"}));
  EXPECT_EQ(replies, 0);
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(second_root), 0u);
  EXPECT_FALSE(std::filesystem::exists(second_root));
}
