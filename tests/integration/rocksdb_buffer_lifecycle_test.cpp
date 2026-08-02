// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
          if (sink(key, payload, std::move(encoding))) return sitos::Result<void>::Ok();
          return sitos::Result<void>::Err(std::make_error_code(std::errc::broken_pipe));
        });
    query.keyexpr = std::string(keyexpr);
    if (queryable_) queryable_(query);
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

  void PutSample(std::string key, std::vector<std::byte> payload) {
    subscriber_(sitos::TransportSample{std::move(key), payload, {"zenoh/bytes"}, std::nullopt,
                                       sitos::TransportSample::Kind::Put});
  }

 private:
  std::function<void(const sitos::TransportSample&)> subscriber_;
  std::function<void(sitos::TransportQuery&)> queryable_;
};

TEST(RocksDBBufferLifecycleTest, CloseReleasesEngineBeforeReturn) {
  RocksDbTransport transport;
  const auto root = std::filesystem::temp_directory_path() / "sitos-issue56-rocksdb-close";
  std::filesystem::remove_all(root);
  auto base = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      base, {.prefix = "sitos",
             .durable_buffer_engine_factory = [&](std::string_view) {
               return sitos::RocksDBEngine::Open((root / "session").string());
             }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}});
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(root), 0u);
  EXPECT_FALSE(std::filesystem::exists(root));
}

TEST(RocksDBBufferLifecycleTest, SameSidRecreationUsesFreshEngine) {
  RocksDbTransport transport;
  const auto root = std::filesystem::temp_directory_path() / "sitos-issue56-rocksdb-recreate";
  std::filesystem::remove_all(root);
  auto base = std::make_shared<sitos::InMemoryEngine>();
  int factory_calls = 0;
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      base, {.prefix = "sitos",
             .durable_buffer_engine_factory = [&](std::string_view) {
               ++factory_calls;
               return sitos::RocksDBEngine::Open((root / ("session-" + std::to_string(factory_calls))).string());
             }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}});
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(factory_calls, 2);
  EXPECT_EQ(transport.Get("sitos/buffers/session/durable/key",
                          [](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                            return true;
                          },
                          std::chrono::milliseconds(500))
                .IsOk());
  EXPECT_TRUE(node.CloseSession("session").IsOk());
  std::filesystem::remove_all(root);
}

}  // namespace
