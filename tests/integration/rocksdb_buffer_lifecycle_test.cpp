// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
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
        std::move(key), payload, {"zenoh/bytes"}, {}, sitos::TransportSample::Kind::Put});
    return sitos::Result<void>::Ok();
  }

 private:
  std::function<void(const sitos::TransportSample&)> subscriber_;
  std::function<void(sitos::TransportQuery&)> queryable_;
};

struct Reply {
  std::string key;
  std::vector<std::byte> payload;
  std::string encoding;
};

struct GateState {
  mutable std::mutex mutex;
  std::condition_variable condition;
  bool block_put = false;
  bool block_explicit_get = false;
  bool block_list = false;
  int put_entered = 0;
  int explicit_get_entered = 0;
  int list_entered = 0;
  bool put_completed = false;
  bool explicit_get_completed = false;
  bool list_completed = false;
};

class BlockingRocksDbEngine final : public sitos::StorageEngine {
 public:
  BlockingRocksDbEngine(std::unique_ptr<sitos::RocksDBEngine> engine,
                        std::shared_ptr<GateState> gates,
                        std::shared_ptr<std::atomic<bool>> destroyed)
      : engine_(std::move(engine)), gates_(std::move(gates)), destroyed_(std::move(destroyed)) {}

  ~BlockingRocksDbEngine() override { destroyed_->store(true, std::memory_order_release); }

  bool Put(std::string_view key, sitos::Bytes value) override {
    const bool tracked = Wait(gates_->block_put, gates_->put_entered);
    const bool result = engine_->Put(key, value);
    if (tracked) Complete(gates_->put_completed);
    return result;
  }

  bool Delete(std::string_view key) override { return engine_->Delete(key); }

  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    const bool designated = key == "seed";
    const bool tracked =
        designated && Wait(gates_->block_explicit_get, gates_->explicit_get_entered);
    const bool result = engine_->Get(key, sink);
    if (tracked) Complete(gates_->explicit_get_completed);
    return result;
  }

  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    const bool tracked = Wait(gates_->block_list, gates_->list_entered);
    const bool result = engine_->List(prefix, sink);
    if (tracked) Complete(gates_->list_completed);
    return result;
  }

  std::shared_ptr<const sitos::StorageReader> TakeSnapshot() const override {
    return engine_->TakeSnapshot();
  }

 private:
  bool Wait(bool& blocked, int& entered) const {
    std::unique_lock lock(gates_->mutex);
    if (!blocked) return false;
    ++entered;
    gates_->condition.notify_all();
    gates_->condition.wait(lock, [&] { return !blocked; });
    return true;
  }

  void Complete(bool& completed) const {
    {
      std::scoped_lock lock(gates_->mutex);
      completed = true;
    }
    gates_->condition.notify_all();
  }

  std::unique_ptr<sitos::RocksDBEngine> engine_;
  std::shared_ptr<GateState> gates_;
  std::shared_ptr<std::atomic<bool>> destroyed_;
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
  explicit ThreadScope(std::shared_ptr<GateState> gates) : gates_(std::move(gates)) {
    threads_.reserve(4);
  }

  ~ThreadScope() {
    Release();
    JoinAll();
  }

  template <typename Function>
  void Start(Function&& function) {
    if (threads_.size() == threads_.capacity()) {
      throw std::logic_error("RocksDB lifecycle test exceeded thread-scope capacity");
    }
    threads_.emplace_back(std::forward<Function>(function));
  }

  void ReleasePut() {
    {
      std::scoped_lock lock(gates_->mutex);
      gates_->block_put = false;
    }
    gates_->condition.notify_all();
  }

  void ReleaseGet() {
    {
      std::scoped_lock lock(gates_->mutex);
      gates_->block_explicit_get = false;
    }
    gates_->condition.notify_all();
  }

  void ReleaseList() {
    {
      std::scoped_lock lock(gates_->mutex);
      gates_->block_list = false;
    }
    gates_->condition.notify_all();
  }

  void Release() {
    ReleasePut();
    ReleaseGet();
    ReleaseList();
  }

  void JoinAll() {
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

 private:
  std::shared_ptr<GateState> gates_;
  std::vector<std::thread> threads_;
};

bool WaitForMaterialization(std::chrono::milliseconds timeout,
                            const std::shared_ptr<GateState>& gates) {
  std::unique_lock lock(gates->mutex);
  return gates->condition.wait_for(lock, timeout, [&] {
    return gates->put_entered > 0 && gates->explicit_get_entered > 0 && gates->list_entered > 0;
  });
}

bool WaitForCompletion(std::chrono::milliseconds timeout, const std::shared_ptr<GateState>& gates,
                       bool GateState::*completed) {
  std::unique_lock lock(gates->mutex);
  return gates->condition.wait_for(lock, timeout, [&] { return gates.get()->*completed; });
}

bool EnsureDirectory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) return false;
  const bool directory = std::filesystem::is_directory(path, error);
  return directory && !error;
}

}  // namespace

TEST(RocksDBBufferLifecycleTest, CloseReleasesEngineBeforeReturn) {
  RocksDbTransport transport;
  ScopedRoot root("close");
  auto gates = std::make_shared<GateState>();
  auto destroyed = std::make_shared<std::atomic<bool>>(false);
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
  ASSERT_TRUE(transport.PutSample("sitos/buffers/session/durable/seed", {std::byte{3}}).IsOk());
  {
    std::scoped_lock lock(gates->mutex);
    gates->block_put = true;
    gates->block_explicit_get = true;
    gates->block_list = true;
  }

  std::optional<sitos::Result<void>> put_result;
  std::optional<sitos::Result<void>> get_result;
  std::optional<sitos::Result<void>> list_result;
  std::vector<Reply> get_replies;
  std::vector<Reply> list_replies;
  ThreadScope threads(gates);
  threads.Start([&] {
    put_result = transport.PutSample("sitos/buffers/session/durable/written", {std::byte{7}});
  });
  threads.Start([&] {
    get_result = transport.Get(
        "sitos/buffers/session/durable/seed",
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          get_replies.push_back(
              {std::string(key), {payload.begin(), payload.end()}, std::move(encoding.id)});
          return true;
        },
        std::chrono::seconds(1));
  });
  threads.Start([&] {
    list_result = transport.Get(
        "sitos/buffers/session/durable/**",
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          list_replies.push_back(
              {std::string(key), {payload.begin(), payload.end()}, std::move(encoding.id)});
          return true;
        },
        std::chrono::seconds(1));
  });

  if (!WaitForMaterialization(std::chrono::seconds(5), gates)) {
    threads.Release();
    threads.JoinAll();
    ADD_FAILURE() << "designated real-engine Put/Get/List operations did not all enter barriers";
    return;
  }

  bool put_completed_before_workers = false;
  bool get_completed_before_workers = false;
  bool list_completed_before_workers = false;
  {
    std::scoped_lock lock(gates->mutex);
    put_completed_before_workers = gates->put_completed;
    get_completed_before_workers = gates->explicit_get_completed;
    list_completed_before_workers = gates->list_completed;
  }
  EXPECT_FALSE(put_completed_before_workers);
  EXPECT_FALSE(get_completed_before_workers);
  EXPECT_FALSE(list_completed_before_workers);

  std::optional<sitos::Result<void>> close_result;
  std::atomic<bool> close_done = false;
  std::atomic<bool> destroyed_at_close_return = false;
  threads.Start([&] {
    close_result = node.CloseSession("session");
    destroyed_at_close_return.store(destroyed->load(std::memory_order_acquire),
                                    std::memory_order_release);
    close_done.store(true, std::memory_order_release);
  });
  const bool closing_seen =
      sitos::storage_node_test_access::StorageNodeTestAccess::WaitForClosing(node, "session");
  const bool close_done_while_blocked = close_done.load(std::memory_order_acquire);

  threads.ReleasePut();
  if (!WaitForCompletion(std::chrono::seconds(5), gates, &GateState::put_completed)) {
    threads.Release();
    threads.JoinAll();
    ADD_FAILURE() << "real RocksDB PUT did not complete after its gate was released";
    return;
  }
  const bool close_done_after_put = close_done.load(std::memory_order_acquire);
  const bool destroyed_after_put = destroyed->load(std::memory_order_acquire);
  EXPECT_FALSE(close_done_after_put);
  EXPECT_FALSE(destroyed_after_put);

  threads.ReleaseGet();
  if (!WaitForCompletion(std::chrono::seconds(5), gates, &GateState::explicit_get_completed)) {
    threads.Release();
    threads.JoinAll();
    ADD_FAILURE() << "designated RocksDB Get did not complete after its gate was released";
    return;
  }
  const bool close_done_after_get = close_done.load(std::memory_order_acquire);
  const bool destroyed_after_get = destroyed->load(std::memory_order_acquire);
  EXPECT_FALSE(close_done_after_get);
  EXPECT_FALSE(destroyed_after_get);

  threads.ReleaseList();
  if (!WaitForCompletion(std::chrono::seconds(5), gates, &GateState::list_completed)) {
    threads.Release();
    threads.JoinAll();
    ADD_FAILURE() << "designated RocksDB List did not complete after its gate was released";
    return;
  }
  threads.JoinAll();

  bool get_completed = false;
  bool list_completed = false;
  {
    std::scoped_lock lock(gates->mutex);
    get_completed = gates->explicit_get_completed;
    list_completed = gates->list_completed;
  }
  EXPECT_TRUE(closing_seen);
  EXPECT_FALSE(close_done_while_blocked);
  EXPECT_TRUE(get_completed);
  EXPECT_TRUE(list_completed);
  ASSERT_TRUE(put_result.has_value());
  ASSERT_TRUE(get_result.has_value());
  ASSERT_TRUE(list_result.has_value());
  ASSERT_TRUE(close_result.has_value());
  EXPECT_TRUE(put_result->IsOk());
  EXPECT_TRUE(get_result->IsOk());
  EXPECT_TRUE(list_result->IsOk());
  EXPECT_TRUE(close_result->IsOk());
  EXPECT_TRUE(destroyed_at_close_return.load(std::memory_order_acquire));
  EXPECT_EQ(get_replies.size(), 1u);
  ASSERT_EQ(get_replies.size(), 1u);
  EXPECT_EQ(get_replies[0].key, "sitos/buffers/session/durable/seed");
  EXPECT_EQ(get_replies[0].payload, (std::vector<std::byte>{std::byte{3}}));
  EXPECT_EQ(get_replies[0].encoding, "zenoh/bytes");

  std::map<std::string, Reply> listed;
  for (const auto& reply : list_replies) {
    ASSERT_TRUE(listed.emplace(reply.key, reply).second);
  }
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed.at("sitos/buffers/session/durable/seed").payload,
            (std::vector<std::byte>{std::byte{3}}));
  EXPECT_EQ(listed.at("sitos/buffers/session/durable/written").payload,
            (std::vector<std::byte>{std::byte{7}}));
  EXPECT_EQ(listed.at("sitos/buffers/session/durable/seed").encoding, "zenoh/bytes");
  EXPECT_EQ(listed.at("sitos/buffers/session/durable/written").encoding, "zenoh/bytes");
  EXPECT_TRUE(destroyed->load(std::memory_order_acquire));
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
                   std::make_unique<BlockingRocksDbEngine>(
                       std::move(opened).Value(), std::make_shared<GateState>(),
                       std::make_shared<std::atomic<bool>>(false)));
             }}));
  ASSERT_TRUE(EnsureDirectory(first_root));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}}).IsOk());
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(first_root), 0u);
  EXPECT_FALSE(std::filesystem::exists(first_root));
  ASSERT_TRUE(EnsureDirectory(second_root));

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
