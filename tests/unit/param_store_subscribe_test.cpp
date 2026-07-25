// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<void> Get(std::string_view, const QueryResultSink&, std::chrono::milliseconds) override {
    return sitos::Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
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
  std::function<void(const sitos::TransportSample&)> subscriber;
  std::optional<sitos::TransportSample> sample_during_declaration;
  std::optional<sitos::Result<sitos::Subscription>> declaration_error;
};

sitos::TransportSample MakePutSample() {
  const auto payload = sitos::ParamValue(true).Encode();
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
