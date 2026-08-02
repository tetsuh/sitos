// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/storage_node.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

class ProgrammableEngine final : public StorageEngine {
 public:
  explicit ProgrammableEngine(std::shared_ptr<bool> destroyed = nullptr)
      : destroyed_(std::move(destroyed)) {}
  ~ProgrammableEngine() override {
    if (destroyed_) *destroyed_ = true;
  }

  bool Put(std::string_view key, Bytes value) override {
    std::unique_lock lock(mutex_);
    ++put_calls;
    if (block_put) {
      ++put_entered;
      cv.notify_all();
      cv.wait(lock, [this] { return !block_put; });
    }
    if (throw_put) throw std::runtime_error("put failure");
    if (false_puts > 0) {
      --false_puts;
      if (partial_false_put) values_[std::string(key)] = Copy(value);
      return false;
    }
    values_[std::string(key)] = Copy(value);
    return true;
  }

  bool Delete(std::string_view key) override {
    std::scoped_lock lock(mutex_);
    values_.erase(std::string(key));
    return true;
  }

  bool Get(std::string_view key, const EntrySink& sink) const override {
    std::unique_lock lock(mutex_);
    ++get_calls;
    if (block_get) {
      ++get_entered;
      cv.notify_all();
      cv.wait(lock, [this] { return !block_get; });
    }
    if (throw_get) throw std::runtime_error("get failure");
    auto it = values_.find(std::string(key));
    if (it == values_.end()) return false;
    auto copied_key = it->first;
    auto copied_value = it->second;
    lock.unlock();
    return sink(copied_key, copied_value);
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    std::unique_lock lock(mutex_);
    ++list_calls;
    if (block_list) {
      ++list_entered;
      cv.notify_all();
      cv.wait(lock, [this] { return !block_list; });
    }
    if (throw_list) throw std::runtime_error("list failure");
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    for (const auto& [key, value] : values_) {
      if (key.starts_with(prefix)) entries.emplace_back(key, value);
    }
    lock.unlock();
    for (const auto& [key, value] : entries) {
      if (!sink(key, value)) return false;
    }
    return !false_list;
  }

  void WaitFor(std::condition_variable& condition, int& count) {
    std::unique_lock lock(mutex_);
    condition.wait(lock, [&] { return count > 0; });
  }
  void ReleasePut() { SetBlocked(block_put, false); }
  void ReleaseGet() { SetBlocked(block_get, false); }
  void ReleaseList() { SetBlocked(block_list, false); }

  mutable std::mutex mutex_;
  mutable std::condition_variable cv;
  mutable std::map<std::string, std::vector<std::byte>> values_;
  mutable int put_calls = 0;
  mutable int get_calls = 0;
  mutable int list_calls = 0;
  mutable int put_entered = 0;
  mutable int get_entered = 0;
  mutable int list_entered = 0;
  bool block_put = false;
  bool block_get = false;
  bool block_list = false;
  bool throw_put = false;
  mutable bool throw_get = false;
  mutable bool throw_list = false;
  mutable bool false_list = false;
  int false_puts = 0;
  bool partial_false_put = false;
  std::shared_ptr<bool> destroyed_;

 private:
  static std::vector<std::byte> Copy(Bytes value) { return {value.begin(), value.end()}; }
  void SetBlocked(bool& blocked, bool value) {
    {
      std::scoped_lock lock(mutex_);
      blocked = value;
    }
    cv.notify_all();
  }
};

class BufferTransport final : public Transport {
 public:
  struct QueryResult {
    std::vector<std::pair<std::string, std::vector<std::byte>>> replies;
    std::vector<Encoding> encodings;
    int reply_calls = 0;
  };

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

  void PutSample(std::string key, std::vector<std::byte> payload,
                 std::string encoding = "zenoh/bytes") {
    TransportSample sample{std::move(key), payload, Encoding{std::move(encoding)}, std::nullopt,
                           TransportSample::Kind::Put};
    subscriber(sample);
  }
  void DeleteSample(std::string key) {
    TransportSample sample{
        std::move(key), {}, Encoding{"zenoh/bytes"}, std::nullopt, TransportSample::Kind::Delete};
    subscriber(sample);
  }
  QueryResult Query(std::string key, int fail_after = -1, bool throw_reply = false) {
    QueryResult result;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view reply_key, std::span<const std::byte> bytes, Encoding encoding) {
          if (fail_after >= 0 && result.reply_calls >= fail_after) {
            if (throw_reply) throw std::runtime_error("reply failure");
            return Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
          }
          ++result.reply_calls;
          result.replies.emplace_back(std::string(reply_key),
                                      std::vector<std::byte>(bytes.begin(), bytes.end()));
          result.encodings.push_back(std::move(encoding));
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(key);
    queryable(query);
    return result;
  }

  std::function<void(const TransportSample&)> subscriber;
  std::function<void(TransportQuery&)> queryable;
};

struct StorageNodeBufferRoutingTest : testing::Test {
  void SetUp() override {
    ASSERT_TRUE(node.Start(base, transport,
                           {.prefix = "sitos",
                            .log_sink = nullptr,
                            .durable_buffer_engine_factory = [&](std::string_view sid) {
                              factory_sids.emplace_back(sid);
                              auto engine = std::make_unique<ProgrammableEngine>();
                              auto* pointer = engine.get();
                              engines.emplace(std::string(sid), pointer);
                              return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(engine));
                            }}));
  }
  std::shared_ptr<ProgrammableEngine> base = std::make_shared<ProgrammableEngine>();
  BufferTransport transport;
  StorageNode node{transport};
  std::vector<std::string> factory_sids;
  std::map<std::string, ProgrammableEngine*> engines;
};

TEST_F(StorageNodeBufferRoutingTest, CapabilityMatrix) {
  ASSERT_TRUE(node.CreateSession("none").IsOk());
  ASSERT_TRUE(node.CreateSession("dur", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node.CreateSession("eph", {.ephemeral_buffers = true}).IsOk());
  ASSERT_TRUE(
      node.CreateSession("both", {.durable_buffers = true, .ephemeral_buffers = true}).IsOk());
  EXPECT_EQ(factory_sids, (std::vector<std::string>{"dur", "both"}));

  transport.PutSample("sitos/buffers/none/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/dur/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/eph/ephemeral/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/both/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/both/ephemeral/k", {std::byte{1}});
  EXPECT_EQ(engines.at("dur")->put_calls, 1);
  EXPECT_EQ(engines.at("both")->put_calls, 1);
  EXPECT_EQ(transport.Query("sitos/buffers/eph/ephemeral/**").replies.size(), 0u);
}

TEST_F(StorageNodeBufferRoutingTest, DurablePutIsByteExactAndWriteOnce) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  const std::vector<std::byte> value{std::byte{1}, std::byte{2}, std::byte{0}};
  transport.PutSample("sitos/buffers/s/durable/k", value);
  transport.PutSample("sitos/buffers/s/durable/k", value);
  auto result = transport.Query("sitos/buffers/s/durable/k");
  ASSERT_EQ(result.replies.size(), 1u);
  EXPECT_EQ(result.replies[0].second, value);
  ASSERT_EQ(result.encodings.size(), 1u);
  EXPECT_EQ(result.encodings[0].id, "zenoh/bytes");
  EXPECT_EQ(engines.at("s")->put_calls, 1);
}

TEST_F(StorageNodeBufferRoutingTest, PutFailureRereadsAuthoritativeEngineState) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  engine->false_puts = 1;
  engine->partial_false_put = true;
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_EQ(engine->put_calls, 1);
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_EQ(engine->put_calls, 1);
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{2}});
  EXPECT_EQ(engine->put_calls, 1);
  auto result = transport.Query("sitos/buffers/s/durable/k");
  ASSERT_EQ(result.replies.size(), 1u);
  EXPECT_EQ(result.replies[0].second, (std::vector<std::byte>{std::byte{1}}));
}

TEST_F(StorageNodeBufferRoutingTest, WholeSubscriberSerializationPreventsConflictingPuts) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  std::thread first([&] { transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}); });
  std::thread second([&] { transport.PutSample("sitos/buffers/s/durable/k", {std::byte{2}}); });
  first.join();
  second.join();
  auto* engine = engines.at("s");
  EXPECT_EQ(engine->put_calls, 1);
  auto result = transport.Query("sitos/buffers/s/durable/k");
  ASSERT_EQ(result.replies.size(), 1u);
  EXPECT_TRUE(result.replies[0].second == std::vector<std::byte>{std::byte{1}} ||
              result.replies[0].second == std::vector<std::byte>{std::byte{2}});
}

TEST_F(StorageNodeBufferRoutingTest, EphemeralPutNeverTouchesEngine) {
  ASSERT_TRUE(node.CreateSession("s", {.ephemeral_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/s/ephemeral/k", {std::byte{1}});
  EXPECT_TRUE(engines.empty());
  EXPECT_EQ(transport.Query("sitos/buffers/s/ephemeral/k").replies.size(), 0u);
}

TEST_F(StorageNodeBufferRoutingTest, NonBytesEncodingIsRejected) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true, .ephemeral_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  const std::vector<std::string> rejected = {"sitos.v1", "sitos.v1.batch", "zenoh/bytes;schema=x",
                                             "", "application/octet-stream"};
  for (const auto& encoding : rejected) {
    transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}, encoding);
  }
  EXPECT_EQ(engine->get_calls, 0);
  EXPECT_EQ(engine->put_calls, 0);
  transport.PutSample("sitos/buffers/s/durable/empty", {}, "zenoh/bytes");
  EXPECT_EQ(engine->put_calls, 1);
}

TEST_F(StorageNodeBufferRoutingTest, DurableQuerySelectorsAndFailures) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/a/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable/a/l", {std::byte{2}});
  transport.PutSample("sitos/buffers/s/durable/b", {std::byte{3}});
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/a/k").replies.size(), 1u);
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/**").replies.size(), 3u);
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/a/**").replies.size(), 2u);
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/missing").replies.size(), 0u);
  EXPECT_EQ(transport.Query("sitos/buffers/s/ephemeral/**").replies.size(), 0u);

  auto* engine = engines.at("s");
  engine->false_list = true;
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/**").replies.empty());
  engine->false_list = false;
  engine->throw_get = true;
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/a/k").replies.empty());
  engine->throw_get = false;
  auto partial = transport.Query("sitos/buffers/s/durable/**", 1);
  EXPECT_EQ(partial.replies.size(), 1u);
  EXPECT_EQ(partial.reply_calls, 1);
}

TEST_F(StorageNodeBufferRoutingTest, EngineFailuresAndExceptionsAreContained) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  engine->throw_put = true;
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}));
  EXPECT_EQ(engine->put_calls, 1);
  engine->throw_put = false;
  engine->throw_get = true;
  EXPECT_NO_THROW(transport.Query("sitos/buffers/s/durable/k"));
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/k", {std::byte{2}}));
}

TEST_F(StorageNodeBufferRoutingTest, BufferRoutesDoNotEnterParameterSurfaces) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_TRUE(transport.Query("sitos/session/s/k").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/meta/session/s").replies.empty() == false);
}

TEST_F(StorageNodeBufferRoutingTest, BufferDeleteAndControlRoutesAreRejected) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  EXPECT_NO_THROW(transport.DeleteSample("sitos/buffers/s/durable/k"));
  transport.PutSample("sitos/buffers/s/durable/k/*", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable", {std::byte{1}});
  EXPECT_EQ(engine->put_calls, 0);
}

}  // namespace
}  // namespace sitos
