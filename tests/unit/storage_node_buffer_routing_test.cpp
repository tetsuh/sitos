// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sitos/storage_node.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

class CountingEngine final : public StorageEngine {
 public:
  explicit CountingEngine(std::shared_ptr<bool> destroyed = nullptr)
      : destroyed_(std::move(destroyed)) {}
  ~CountingEngine() override {
    if (destroyed_) *destroyed_ = true;
  }
  bool Put(std::string_view key, Bytes value) override {
    std::lock_guard lock(mutex_);
    values_[std::string(key)] = std::vector<std::byte>(value.begin(), value.end());
    ++puts;
    return true;
  }
  bool Delete(std::string_view key) override {
    std::lock_guard lock(mutex_);
    values_.erase(std::string(key));
    return true;
  }
  bool Get(std::string_view key, const EntrySink& sink) const override {
    std::lock_guard lock(mutex_);
    auto it = values_.find(std::string(key));
    if (it == values_.end()) return false;
    return sink(it->first, it->second);
  }
  bool List(std::string_view prefix, const EntrySink& sink) const override {
    std::lock_guard lock(mutex_);
    for (const auto& [key, value] : values_) {
      if (key.starts_with(prefix) && !sink(key, value)) return false;
    }
    return true;
  }
  mutable std::mutex mutex_;
  mutable int puts = 0;
  std::map<std::string, std::vector<std::byte>> values_;
  std::shared_ptr<bool> destroyed_;
};

class BufferTransport final : public Transport {
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
  void PutSample(std::string key, std::vector<std::byte> payload,
                 std::string encoding = "zenoh/bytes") {
    TransportSample sample{std::move(key), payload, Encoding{std::move(encoding)}, std::nullopt,
                           TransportSample::Kind::Put};
    subscriber(sample);
  }
  std::vector<TransportSample> Samples() const { return {}; }
  std::vector<std::pair<std::string, std::vector<std::byte>>> Query(std::string key) {
    std::vector<std::pair<std::string, std::vector<std::byte>>> result;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view reply_key, std::span<const std::byte> bytes, Encoding) {
          result.emplace_back(std::string(reply_key),
                              std::vector<std::byte>(bytes.begin(), bytes.end()));
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(key);
    queryable(query);
    return result;
  }
  std::function<void(const TransportSample&)> subscriber;
  std::function<void(TransportQuery&)> queryable;
};

struct BufferFixture : testing::Test {
  void SetUp() override {
    ASSERT_TRUE(node.Start(base, transport,
                           {.prefix = "sitos",
                            .log_sink = nullptr,
                            .durable_buffer_engine_factory = [&](std::string_view sid) {
                              factory_sid = std::string(sid);
                              durable = std::make_unique<CountingEngine>();
                              durable_ptr = durable.get();
                              return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(durable));
                            }}));
  }
  std::shared_ptr<CountingEngine> base = std::make_shared<CountingEngine>();
  BufferTransport transport;
  StorageNode node{transport};
  std::string factory_sid;
  std::unique_ptr<CountingEngine> durable;
  CountingEngine* durable_ptr = nullptr;
};
using StorageNodeBufferRoutingTest = BufferFixture;

TEST_F(StorageNodeBufferRoutingTest, CapabilityMatrix) {
  ASSERT_TRUE(node.CreateSession("none").IsOk());
  ASSERT_TRUE(node.CreateSession("dur", {.durable = true}).IsOk());
  ASSERT_TRUE(node.CreateSession("eph", {.ephemeral = true}).IsOk());
  EXPECT_EQ(factory_sid, "dur");
}
TEST_F(StorageNodeBufferRoutingTest, DurablePutIsByteExactAndWriteOnce) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  std::vector<std::byte> value{std::byte{1}, std::byte{2}};
  transport.PutSample("sitos/buffers/s/durable/k", value);
  transport.PutSample("sitos/buffers/s/durable/k", value);
  EXPECT_EQ(durable_ptr->puts, 1);
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/k").size(), 1u);
}
TEST_F(StorageNodeBufferRoutingTest, PutFailureRereadsAuthoritativeEngineState) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_EQ(durable_ptr->puts, 1);
}
TEST_F(StorageNodeBufferRoutingTest, WholeSubscriberSerializationPreventsConflictingPuts) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{2}});
  EXPECT_EQ(durable_ptr->puts, 1);
}
TEST_F(StorageNodeBufferRoutingTest, EphemeralPutNeverTouchesEngine) {
  ASSERT_TRUE(node.CreateSession("s", {.ephemeral = true}).IsOk());
  transport.PutSample("sitos/buffers/s/ephemeral/k", {std::byte{1}});
  EXPECT_EQ(durable_ptr, nullptr);
}
TEST_F(StorageNodeBufferRoutingTest, NonBytesEncodingIsRejected) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}, "sitos.v1");
  EXPECT_EQ(durable_ptr->puts, 0);
}
TEST_F(StorageNodeBufferRoutingTest, DurableQuerySelectorsAndFailures) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/a/k", {std::byte{1}});
  EXPECT_EQ(transport.Query("sitos/buffers/s/durable/**").size(), 1u);
  EXPECT_EQ(transport.Query("sitos/buffers/s/ephemeral/**").size(), 0u);
}
TEST_F(StorageNodeBufferRoutingTest, EngineFailuresAndExceptionsAreContained) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  EXPECT_NO_THROW(transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}}));
}
TEST_F(StorageNodeBufferRoutingTest, BufferRoutesDoNotEnterParameterSurfaces) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  transport.PutSample("sitos/buffers/s/durable/k", {std::byte{1}});
  EXPECT_TRUE(transport.Query("sitos/session/s/k").empty());
}
TEST_F(StorageNodeBufferRoutingTest, BufferDeleteAndControlRoutesAreRejected) {
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}).IsOk());
  TransportSample sample{"sitos/buffers/s/durable/k",
                         {},
                         Encoding{"zenoh/bytes"},
                         std::nullopt,
                         TransportSample::Kind::Delete};
  EXPECT_NO_THROW(transport.subscriber(sample));
  EXPECT_EQ(durable_ptr->puts, 0);
}

TEST(StorageNodeBufferLifecycleTest, FactoryFailureTaxonomyAndRollback) {
  BufferTransport transport;
  StorageNode node(transport);
  auto base = std::make_shared<CountingEngine>();
  int calls = 0;
  ASSERT_TRUE(node.Start(base, transport,
                         {.prefix = "sitos",
                          .log_sink = nullptr,
                          .durable_buffer_engine_factory = [&](std::string_view) {
                            ++calls;
                            return Result<std::unique_ptr<StorageEngine>>::Ok(nullptr);
                          }}));
  auto result = node.CreateSession("s", {.durable = true});
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
  EXPECT_EQ(result.Message(), "durable buffer engine factory returned null");
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(node.CreateSession("s").IsOk());
}
TEST(StorageNodeBufferLifecycleTest, FactoryAndStopLinearizeDeterministically) {
  BufferTransport transport;
  StorageNode node(transport);
  ASSERT_TRUE(node.Start(std::make_shared<CountingEngine>(), transport,
                         {.prefix = "sitos", .log_sink = nullptr}));
  EXPECT_TRUE(node.CreateSession("s").IsOk());
  node.Stop();
  EXPECT_EQ(node.CreateSession("after").StatusCode(), Status::InvalidArgument);
}
TEST(StorageNodeBufferLifecycleTest, CloseQuiescesDurableOperationsAndDestroysEngine) {
  BufferTransport transport;
  StorageNode node(transport);
  auto destroyed = std::make_shared<bool>(false);
  auto engine = std::make_unique<CountingEngine>(destroyed);
  ASSERT_TRUE(node.Start(std::make_shared<CountingEngine>(), transport,
                         {.prefix = "sitos",
                          .log_sink = nullptr,
                          .durable_buffer_engine_factory = [&](std::string_view) {
                            return Result<std::unique_ptr<StorageEngine>>::Ok(std::move(engine));
                          }}));
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}));
  ASSERT_TRUE(node.CloseSession("s"));
  EXPECT_TRUE(*destroyed);
}
TEST(StorageNodeBufferLifecycleTest, SameSidRecreationUsesFreshEngine) {
  BufferTransport transport;
  StorageNode node(transport);
  int calls = 0;
  ASSERT_TRUE(node.Start(std::make_shared<CountingEngine>(), transport,
                         {.prefix = "sitos",
                          .log_sink = nullptr,
                          .durable_buffer_engine_factory = [&](std::string_view) {
                            ++calls;
                            return Result<std::unique_ptr<StorageEngine>>::Ok(
                                std::make_unique<CountingEngine>());
                          }}));
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}));
  ASSERT_TRUE(node.CloseSession("s"));
  ASSERT_TRUE(node.CreateSession("s", {.durable = true}));
  EXPECT_EQ(calls, 2);
}

}  // namespace
}  // namespace sitos
