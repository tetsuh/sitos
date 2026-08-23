// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
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
#include "storage_node_test_access.hpp"
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
    WaitUntilReleased(block_put, put_entered);
    std::scoped_lock lock(mutex_);
    ++put_calls;
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
    ++delete_calls;
    values_.erase(std::string(key));
    return true;
  }

  bool Get(std::string_view key, const EntrySink& sink) const override {
    WaitUntilReleased(block_get, get_entered);
    std::unique_lock lock(mutex_);
    ++get_calls;
    if (throw_get) throw std::runtime_error("get failure");
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
    ++list_calls;
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

  void WaitFor(int& count) {
    std::unique_lock lock(gate_mutex_);
    cv.wait(lock, [&] { return count > 0; });
  }
  void ReleasePut() { SetBlocked(block_put, false); }
  void ReleaseGet() { SetBlocked(block_get, false); }
  void ReleaseList() { SetBlocked(block_list, false); }

  mutable std::mutex mutex_;
  mutable std::mutex gate_mutex_;
  mutable std::condition_variable cv;
  mutable std::map<std::string, std::vector<std::byte>> values_;
  mutable int put_calls = 0;
  mutable int get_calls = 0;
  mutable int list_calls = 0;
  mutable int delete_calls = 0;
  mutable int put_entered = 0;
  mutable int get_entered = 0;
  mutable int list_entered = 0;
  bool block_put = false;
  mutable bool block_get = false;
  mutable bool block_list = false;
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
      std::scoped_lock lock(gate_mutex_);
      blocked = value;
    }
    cv.notify_all();
  }
  void WaitUntilReleased(bool& blocked, int& entered) const {
    std::unique_lock lock(gate_mutex_);
    if (!blocked) return;
    ++entered;
    cv.notify_all();
    cv.wait(lock, [&] { return !blocked; });
  }
};

class BufferTransport final : public Transport {
 public:
  struct QueryResult {
    std::vector<std::pair<std::string, std::vector<std::byte>>> replies;
    std::vector<Encoding> encodings;
    int reply_calls = 0;
    int reply_attempts = 0;
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
    {
      std::scoped_lock lock(sample_mutex);
      ++sample_callbacks;
      sample_cv.notify_all();
    }
    TransportSample sample{std::move(key), payload, Encoding{std::move(encoding)}, {},
                           TransportSample::Kind::Put};
    subscriber(sample);
  }
  void WaitForSamples(int count) {
    std::unique_lock lock(sample_mutex);
    sample_cv.wait(lock, [&] { return sample_callbacks >= count; });
  }
  void DeleteSample(std::string key) {
    TransportSample sample{
        std::move(key), {}, Encoding{"zenoh/bytes"}, {}, TransportSample::Kind::Delete};
    subscriber(sample);
  }
  QueryResult Query(std::string key, int fail_after = -1, bool throw_reply = false) {
    QueryResult result;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view reply_key, std::span<const std::byte> bytes, Encoding encoding) {
          ++result.reply_attempts;
          if (fail_after >= 0 && result.reply_attempts > fail_after) {
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
  std::mutex sample_mutex;
  std::condition_variable sample_cv;
  int sample_callbacks = 0;
};

class RecordingLogSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::scoped_lock lock(mutex);
    ++write_attempts;
    if (throwing) throw std::runtime_error("log failure");
    messages.emplace_back(record.message);
  }
  std::vector<std::string> Messages() const {
    std::scoped_lock lock(mutex);
    return messages;
  }
  mutable std::mutex mutex;
  std::vector<std::string> messages;
  int write_attempts = 0;
  bool throwing = false;
};

struct StorageNodeBufferRoutingTest : testing::Test {
  void SetUp() override {
    ASSERT_TRUE(node.Start(base, transport,
                           {.prefix = "sitos",
                            .log_sink = log_sink,
                            .durable_buffer_engine_factory = [&](std::string_view sid) {
                              factory_sids.emplace_back(sid);
                              auto engine = std::make_unique<ProgrammableEngine>();
                              auto* pointer = engine.get();
                              engines.emplace(std::string(sid), pointer);
                              return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(engine));
                            }}));
  }
  std::shared_ptr<ProgrammableEngine> base = std::make_shared<ProgrammableEngine>();
  std::shared_ptr<RecordingLogSink> log_sink = std::make_shared<RecordingLogSink>();
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

  const auto enabled_ephemeral_before = log_sink->Messages().size();
  transport.PutSample("sitos/buffers/eph/ephemeral/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/both/ephemeral/k", {std::byte{1}});
  EXPECT_EQ(log_sink->Messages().size(), enabled_ephemeral_before);
  const auto disabled_before = log_sink->Messages().size();
  transport.PutSample("sitos/buffers/none/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/none/ephemeral/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/dur/ephemeral/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/eph/durable/k", {std::byte{1}});
  const auto disabled_messages = log_sink->Messages();
  ASSERT_EQ(disabled_messages.size(), disabled_before + 4);
  for (std::size_t i = disabled_before; i < disabled_messages.size(); ++i) {
    EXPECT_EQ(disabled_messages[i], "buffer capability disabled");
  }
  transport.PutSample("sitos/buffers/dur/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/both/durable/k", {std::byte{1}});
  EXPECT_EQ(engines.at("dur")->put_calls, 1);
  EXPECT_EQ(engines.at("both")->put_calls, 1);
  EXPECT_FALSE(engines.contains("eph"));
  EXPECT_EQ(transport.Query("sitos/buffers/none/ephemeral/**").replies.size(), 0u);
  EXPECT_EQ(transport.Query("sitos/buffers/dur/ephemeral/**").replies.size(), 0u);
  EXPECT_EQ(transport.Query("sitos/buffers/eph/durable/**").replies.size(), 0u);
  EXPECT_EQ(transport.Query("sitos/buffers/both/ephemeral/**").replies.size(), 0u);
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
  const auto put_failure_before = log_sink->Messages().size();
  transport.PutSample("sitos/buffers/s/durable/unmodified", {std::byte{1}});
  auto put_failure_messages = log_sink->Messages();
  ASSERT_EQ(put_failure_messages.size(), put_failure_before + 1);
  EXPECT_EQ(put_failure_messages.back(), "durable buffer PUT failed");
  EXPECT_EQ(engine->put_calls, 1);
  transport.PutSample("sitos/buffers/s/durable/unmodified", {std::byte{1}});
  EXPECT_EQ(engine->put_calls, 2);
  engine->false_puts = 1;
  engine->partial_false_put = true;
  transport.PutSample("sitos/buffers/s/durable/partial", {std::byte{2}});
  EXPECT_EQ(engine->put_calls, 3);
  transport.PutSample("sitos/buffers/s/durable/partial", {std::byte{2}});
  EXPECT_EQ(engine->put_calls, 3);
  transport.PutSample("sitos/buffers/s/durable/partial", {std::byte{3}});
  EXPECT_EQ(engine->put_calls, 3);
  put_failure_messages = log_sink->Messages();
  EXPECT_EQ(put_failure_messages.back(), "durable buffer PUT conflicts with existing value");
  auto result = transport.Query("sitos/buffers/s/durable/partial");
  ASSERT_EQ(result.replies.size(), 1u);
  EXPECT_EQ(result.replies[0].first, "sitos/buffers/s/durable/partial");
  EXPECT_EQ(result.replies[0].second, (std::vector<std::byte>{std::byte{2}}));
}

TEST_F(StorageNodeBufferRoutingTest, WholeSubscriberSerializationPreventsConflictingPuts) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  engine->block_get = true;
  std::mutex observer_mutex;
  std::condition_variable observer_cv;
  int boundary_entries = 0;
  ASSERT_TRUE(
      storage_node_test_access::StorageNodeTestAccess::SetSubscriberEntryObserver(node, [&] {
        {
          std::scoped_lock lock(observer_mutex);
          ++boundary_entries;
        }
        observer_cv.notify_all();
      }));
  std::thread first([&] { transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}); });
  engine->WaitFor(engine->get_entered);
  std::thread second([&] { transport.PutSample("sitos/buffers/s/durable/k", {std::byte{2}}); });
  {
    std::unique_lock lock(observer_mutex);
    observer_cv.wait(lock, [&] { return boundary_entries >= 2; });
  }
  {
    std::scoped_lock lock(engine->gate_mutex_);
    EXPECT_EQ(engine->get_entered, 1);
  }
  EXPECT_FALSE(storage_node_test_access::StorageNodeTestAccess::TryLockSubscriberMutex(node));
  engine->ReleaseGet();
  first.join();
  second.join();
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
  const auto diagnostics_before = log_sink->Messages().size();
  const std::string sentinel_payload = "sentinel-payload";
  for (const auto& encoding : rejected) {
    transport.PutSample("sitos/buffers/s/durable/sentinel-key",
                        {std::byte{'s'}, std::byte{'e'}, std::byte{'n'}, std::byte{'t'},
                         std::byte{'i'}, std::byte{'n'}, std::byte{'e'}, std::byte{'l'}},
                        encoding);
  }
  const auto diagnostics_after = log_sink->Messages();
  ASSERT_GE(diagnostics_after.size(), diagnostics_before + rejected.size());
  for (std::size_t i = diagnostics_before; i < diagnostics_after.size(); ++i) {
    EXPECT_EQ(diagnostics_after[i], "buffer encoding rejected");
    EXPECT_EQ(diagnostics_after[i].find("sentinel"), std::string::npos);
    EXPECT_EQ(diagnostics_after[i].find(sentinel_payload), std::string::npos);
  }
  EXPECT_EQ(engine->get_calls, 0);
  EXPECT_EQ(engine->put_calls, 0);
  log_sink->throwing = true;
  const int subscriber_log_attempts = log_sink->write_attempts;
  EXPECT_NO_THROW(
      transport.PutSample("sitos/buffers/s/durable/throw-log", {std::byte{1}}, "not-bytes"));
  EXPECT_EQ(log_sink->write_attempts, subscriber_log_attempts + 1);
  log_sink->throwing = false;
  EXPECT_EQ(engine->put_calls, 0);
  transport.PutSample("sitos/buffers/s/durable/empty", {}, "zenoh/bytes");
  EXPECT_EQ(engine->put_calls, 1);
}

TEST_F(StorageNodeBufferRoutingTest, DurableQuerySelectorsAndFailures) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/a/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable/a/l", {std::byte{2}});
  transport.PutSample("sitos/buffers/s/durable/b", {std::byte{3}});
  auto exact = transport.Query("sitos/buffers/s/durable/a/k");
  ASSERT_EQ(exact.replies.size(), 1u);
  EXPECT_EQ(exact.replies[0].first, "sitos/buffers/s/durable/a/k");
  EXPECT_EQ(exact.replies[0].second, (std::vector<std::byte>{std::byte{1}}));
  EXPECT_EQ(exact.encodings[0].id, "zenoh/bytes");
  auto root = transport.Query("sitos/buffers/s/durable/**");
  ASSERT_EQ(root.replies.size(), 3u);
  EXPECT_EQ(root.replies[0].first, "sitos/buffers/s/durable/a/k");
  EXPECT_EQ(root.replies[1].first, "sitos/buffers/s/durable/a/l");
  EXPECT_EQ(root.replies[2].first, "sitos/buffers/s/durable/b");
  EXPECT_EQ(root.replies[0].second, (std::vector<std::byte>{std::byte{1}}));
  EXPECT_EQ(root.replies[1].second, (std::vector<std::byte>{std::byte{2}}));
  EXPECT_EQ(root.replies[2].second, (std::vector<std::byte>{std::byte{3}}));
  for (const auto& encoding : root.encodings) EXPECT_EQ(encoding.id, "zenoh/bytes");
  auto subtree = transport.Query("sitos/buffers/s/durable/a/**");
  ASSERT_EQ(subtree.replies.size(), 2u);
  EXPECT_EQ(subtree.replies[0].first, "sitos/buffers/s/durable/a/k");
  EXPECT_EQ(subtree.replies[1].first, "sitos/buffers/s/durable/a/l");
  EXPECT_EQ(subtree.replies[0].second, (std::vector<std::byte>{std::byte{1}}));
  EXPECT_EQ(subtree.replies[1].second, (std::vector<std::byte>{std::byte{2}}));
  ASSERT_EQ(subtree.encodings.size(), 2u);
  EXPECT_EQ(subtree.encodings[0].id, "zenoh/bytes");
  EXPECT_EQ(subtree.encodings[1].id, "zenoh/bytes");
  const auto diagnostics_before_miss = log_sink->Messages().size();
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/missing").replies.size(), 0u);
  EXPECT_EQ(log_sink->Messages().size(), diagnostics_before_miss);
  EXPECT_EQ(transport.Query("sitos/buffers/s/ephemeral/**").replies.size(), 0u);
  ASSERT_TRUE(node.CreateSession("eph", {.ephemeral_buffers = true}).IsOk());
  EXPECT_TRUE(transport.Query("sitos/buffers/eph/durable/**").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/buffers/unknown/durable/**").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/a/*").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/:batch").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/:fence").replies.empty());
  EXPECT_TRUE(transport.Query("sitos/snap/s/**").replies.empty());
  ASSERT_TRUE(node.CloseSession("eph").IsOk());
  EXPECT_TRUE(transport.Query("sitos/buffers/eph/durable/**").replies.empty());

  auto* engine = engines.at("s");
  auto diagnostic_count = [&](std::string_view message) {
    const auto messages = log_sink->Messages();
    return static_cast<int>(std::count(messages.begin(), messages.end(), message));
  };
  engine->false_list = true;
  const int false_list_before = diagnostic_count("durable buffer query failed");
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/**").replies.empty());
  EXPECT_EQ(diagnostic_count("durable buffer query failed"), false_list_before + 1);
  engine->false_list = false;
  engine->throw_list = true;
  const int throw_list_before = diagnostic_count("durable buffer query failed");
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/**").replies.empty());
  EXPECT_EQ(diagnostic_count("durable buffer query failed"), throw_list_before + 1);
  engine->throw_list = false;
  engine->throw_get = true;
  const int throw_get_before = diagnostic_count("durable buffer query failed");
  EXPECT_TRUE(transport.Query("sitos/buffers/s/durable/a/k").replies.empty());
  EXPECT_EQ(diagnostic_count("durable buffer query failed"), throw_get_before + 1);
  engine->throw_get = false;
  auto partial = transport.Query("sitos/buffers/s/durable/**", 1);
  EXPECT_EQ(partial.replies.size(), 1u);
  EXPECT_EQ(partial.reply_calls, 1);
  EXPECT_EQ(partial.reply_attempts, 2);
  const int throwing_handler_before = diagnostic_count("durable buffer query failed");
  const int throwing_handler_attempts = log_sink->write_attempts;
  auto throwing = transport.Query("sitos/buffers/s/durable/**", 1, true);
  EXPECT_EQ(throwing.replies.size(), 1u);
  EXPECT_EQ(throwing.reply_calls, 1);
  EXPECT_EQ(throwing.reply_attempts, 2);
  EXPECT_EQ(diagnostic_count("durable buffer query failed"), throwing_handler_before + 1);
  EXPECT_EQ(log_sink->write_attempts, throwing_handler_attempts + 1);
  const auto throwing_handler_messages = log_sink->Messages();
  ASSERT_EQ(throwing_handler_messages.size(), throwing_handler_before + 1);
  EXPECT_EQ(throwing_handler_messages.back(), "durable buffer query failed");
  EXPECT_EQ(throwing_handler_messages.back().find("sentinel"), std::string::npos);
  const int returned_reply_before = diagnostic_count("durable buffer query failed");
  const int returned_reply_attempts = log_sink->write_attempts;
  auto returned_failure = transport.Query("sitos/buffers/s/durable/**", 1);
  EXPECT_EQ(returned_failure.replies.size(), 1u);
  EXPECT_EQ(returned_failure.reply_attempts, 2);
  EXPECT_EQ(diagnostic_count("durable buffer query failed"), returned_reply_before + 1);
  EXPECT_EQ(log_sink->write_attempts, returned_reply_attempts + 1);
  const int throwing_reply_attempts = log_sink->write_attempts;
  log_sink->throwing = true;
  EXPECT_NO_THROW(transport.Query("sitos/buffers/s/durable/**", 1, true));
  EXPECT_EQ(log_sink->write_attempts, throwing_reply_attempts + 1);
  log_sink->throwing = false;
}

TEST_F(StorageNodeBufferRoutingTest, EngineFailuresAndExceptionsAreContained) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  engine->throw_put = true;
  const auto put_throw_before = log_sink->Messages().size();
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}));
  EXPECT_EQ(engine->put_calls, 1);
  ASSERT_EQ(log_sink->Messages().size(), put_throw_before + 1);
  EXPECT_EQ(log_sink->Messages().back(), "durable buffer PUT failed");
  engine->throw_put = false;
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}));
  EXPECT_EQ(engine->put_calls, 2);
  engine->throw_get = true;
  const auto read_throw_before = log_sink->Messages().size();
  EXPECT_NO_THROW(transport.Query("sitos/buffers/s/durable/k"));
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/read-throw", {std::byte{2}}));
  EXPECT_EQ(log_sink->Messages().size(), read_throw_before + 2);
  EXPECT_EQ(log_sink->Messages()[read_throw_before], "durable buffer query failed");
  EXPECT_EQ(log_sink->Messages()[read_throw_before + 1], "durable buffer PUT failed");
  engine->throw_get = false;
}

TEST_F(StorageNodeBufferRoutingTest, BufferRoutesDoNotEnterParameterSurfaces) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto metadata_before = transport.Query("sitos/meta/session/s");
  auto snapshot_before = transport.Query("sitos/snap/s/**");
  const auto base_values_before = base->values_;
  const int base_gets_before = base->get_calls;
  const int base_lists_before = base->list_calls;
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_TRUE(transport.Query("sitos/session/s/k").replies.empty());
  auto metadata_after = transport.Query("sitos/meta/session/s");
  auto snapshot_after = transport.Query("sitos/snap/s/**");
  ASSERT_EQ(metadata_before.replies.size(), 1u);
  ASSERT_EQ(metadata_after.replies.size(), 1u);
  EXPECT_EQ(metadata_before.replies[0].second, metadata_after.replies[0].second);
  EXPECT_EQ(snapshot_before.replies, snapshot_after.replies);
  EXPECT_EQ(base->values_, base_values_before);
  EXPECT_EQ(base->get_calls, base_gets_before);
  EXPECT_EQ(base->list_calls, base_lists_before);
}

TEST_F(StorageNodeBufferRoutingTest, BufferDeleteAndControlRoutesAreRejected) {
  ASSERT_TRUE(node.CreateSession("s", {.durable_buffers = true}).IsOk());
  auto* engine = engines.at("s");
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{9}});
  const int deletes_before = engine->delete_calls;
  EXPECT_NO_THROW(transport.DeleteSample("sitos/buffers/s/durable/k"));
  EXPECT_EQ(engine->delete_calls, deletes_before);
  auto retained = transport.Query("sitos/buffers/s/durable/k");
  ASSERT_EQ(retained.replies.size(), 1u);
  EXPECT_EQ(retained.replies[0].second, (std::vector<std::byte>{std::byte{9}}));
  transport.PutSample("sitos/buffers/s/durable/k/*", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable/:batch", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable/:fence", {std::byte{1}});
  transport.PutSample("sitos/snap/s/k", {std::byte{1}});
  EXPECT_EQ(engine->put_calls, 1);
}

}  // namespace
}  // namespace sitos
