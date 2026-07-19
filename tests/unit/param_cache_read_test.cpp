// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

class FakeTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                   sitos::Encoding encoding, sitos::PutOptions) override {
    puts.push_back(std::string(key));
    if (sync_delivery && subscriber) {
      sitos::TransportSample sample{std::string(key), payload, std::move(encoding), std::nullopt,
                                    sitos::TransportSample::Kind::Put};
      subscriber(sample);
    }
    return put_result;
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view query, const QueryResultSink& sink,
                   std::chrono::milliseconds) override {
    const bool snapshot = query.find("/snap/") != std::string_view::npos;
    for (const auto& reply : snapshot ? snapshot_replies : overlay_replies) {
      if (!sink(reply.key, reply.payload, reply.encoding)) break;
    }
    return get_result;
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    subscriber = std::move(callback);
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

  struct Reply {
    std::string key;
    std::vector<std::byte> payload;
    sitos::Encoding encoding;
  };

  static Reply Value(std::string key, const sitos::ParamValue& value) {
    return Reply{std::move(key), value.Encode(), sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}};
  }

  std::vector<Reply> snapshot_replies;
  std::vector<Reply> overlay_replies;
  std::vector<std::string> puts;
  std::function<void(const sitos::TransportSample&)> subscriber;
  sitos::Result<void> put_result = sitos::Result<void>::Ok();
  sitos::Result<void> get_result = sitos::Result<void>::Ok();
  bool sync_delivery = false;
};

class ParamCacheReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport = std::make_shared<FakeTransport>();
    auto result = sitos::ParamCache::Open(transport);
    ASSERT_TRUE(result.IsOk());
    cache.emplace(std::move(result).Value());
    transport->snapshot_replies.push_back(FakeTransport::Value("sitos/snap/s1/a", sitos::ParamValue(1)));
    ASSERT_TRUE(cache->Attach("s1").IsOk());
  }

  std::shared_ptr<FakeTransport> transport;
  std::optional<sitos::ParamCache> cache;
};

TEST_F(ParamCacheReadTest, InvalidKeysAndDetachedStateReturnDefinedStatuses) {
  EXPECT_EQ(cache->GetShared("bad/key/").StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(cache->GetShared("missing").StatusCode(), sitos::Status::NotFound);
  cache->Detach();
  EXPECT_EQ(cache->GetShared("a").StatusCode(), sitos::Status::InvalidArgument);
}

TEST_F(ParamCacheReadTest, GetSharedIsLocalAndSurvivesOverwrite) {
  const auto before = cache->GetShared("a");
  ASSERT_TRUE(before.IsOk());
  EXPECT_EQ(before.Value()->As<std::int64_t>(), 1);
  ASSERT_TRUE(cache->Put("a", std::int64_t{2}).IsOk());
  EXPECT_EQ(before.Value()->As<std::int64_t>(), 1);
  EXPECT_EQ(cache->Get<std::int64_t>("a").Value(), 2);
  EXPECT_TRUE(transport->puts.front().find("session/s1/a") != std::string::npos);
}

TEST_F(ParamCacheReadTest, GetAndGetOrUseArithmeticConversionRules) {
  EXPECT_EQ(cache->Get<std::int32_t>("a").Value(), 1);
  EXPECT_EQ(cache->GetOr<std::int64_t>("missing", 9).Value(), 9);
  EXPECT_EQ(cache->GetOr<std::int64_t>("a", 9).Value(), 1);
}

TEST_F(ParamCacheReadTest, SpanHandleSurvivesOverwrite) {
  transport->subscriber = nullptr;
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{1}, std::byte{2}}).IsOk());
  auto handle = cache->GetSpan<std::byte>("bytes");
  ASSERT_TRUE(handle.IsOk());
  ASSERT_EQ(handle.Value().span.size(), 2U);
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{3}}).IsOk());
  EXPECT_EQ(handle.Value().span[0], std::byte{1});
}

TEST_F(ParamCacheReadTest, FailedPutPreservesLocalValueAndStatus) {
  transport->put_result = sitos::Result<void>::Err(sitos::Status::Disconnected, "offline");
  const auto result = cache->Put("a", std::int64_t{9});
  EXPECT_EQ(result.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(result.Message(), "offline");
  EXPECT_EQ(cache->Get<std::int64_t>("a").Value(), 1);
}

TEST_F(ParamCacheReadTest, PutBatchUsesOneCanonicalSubmissionAndPreservesOrder) {
  const std::vector<sitos::BatchEntry> entries{{"b", sitos::ParamValue(2)},
                                                {"a", sitos::ParamValue(1)},
                                                {"b", sitos::ParamValue(3)}};
  ASSERT_TRUE(cache->PutBatch(entries).IsOk());
  ASSERT_EQ(transport->puts.size(), 1U);
  EXPECT_NE(transport->puts.front().find("/session/s1/:batch"), std::string::npos);
  EXPECT_EQ(cache->Get<std::int64_t>("b").Value(), 3);
  EXPECT_EQ(cache->Get<std::int64_t>("a").Value(), 1);
}

TEST_F(ParamCacheReadTest, EmptyPutBatchMakesNoTransportOperation) {
  const std::vector<sitos::BatchEntry> empty;
  ASSERT_TRUE(cache->PutBatch(empty).IsOk());
  EXPECT_TRUE(transport->puts.empty());
}

TEST_F(ParamCacheReadTest, ContainsIsLocalAndDistinguishesAbsence) {
  EXPECT_TRUE(cache->Contains("a").Value());
  EXPECT_FALSE(cache->Contains("missing").Value());
  EXPECT_TRUE(transport->puts.empty());
}

TEST_F(ParamCacheReadTest, ListUsesRawPrefixAndLexicographicOrder) {
  ASSERT_TRUE(cache->Put("foo/z", 3).IsOk());
  ASSERT_TRUE(cache->Put("foobar", 2).IsOk());
  ASSERT_TRUE(cache->Put("foo", 1).IsOk());
  std::vector<std::string> keys;
  ASSERT_TRUE(cache->List("foo", [&](std::string_view key, const sitos::ParamValue&) {
    keys.emplace_back(key);
    return true;
  }).IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo", "foo/z", "foobar"}));
}

TEST_F(ParamCacheReadTest, ListStopsOnFalseAndAllowsReentrantReads) {
  int count = 0;
  auto result = cache->List("", [&](std::string_view key, const sitos::ParamValue&) {
    ++count;
    EXPECT_TRUE(cache->Contains(key).IsOk());
    return false;
  });
  EXPECT_TRUE(result.IsOk());
  EXPECT_EQ(count, 1);
}

TEST_F(ParamCacheReadTest, HotPathPerformsNoTransportOperation) {
  const auto before = transport->puts.size();
  EXPECT_TRUE(cache->GetShared("a").IsOk());
  EXPECT_TRUE(cache->Contains("a").IsOk());
  EXPECT_TRUE(cache->List("a", [](std::string_view, const sitos::ParamValue&) { return true; }).IsOk());
  EXPECT_EQ(transport->puts.size(), before);
}

}  // namespace
