// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "param_cache_test_access.hpp"
#include "sitos/batch.hpp"

namespace {

using Access = sitos::param_cache_test_access::ParamCacheTestAccess;

class FakeTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    ++put_count;
    puts.push_back(std::string(key));
    put_payloads.emplace_back(payload.begin(), payload.end());
    put_encodings.push_back(encoding.id);
    put_options.push_back(options);
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
    ++get_count;
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
  std::vector<std::vector<std::byte>> put_payloads;
  std::vector<std::string> put_encodings;
  std::vector<sitos::PutOptions> put_options;
  std::function<void(const sitos::TransportSample&)> subscriber;
  sitos::Result<void> put_result = sitos::Result<void>::Ok();
  sitos::Result<void> get_result = sitos::Result<void>::Ok();
  std::size_t put_count = 0;
  std::size_t get_count = 0;
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

TEST_F(ParamCacheReadTest, ListRejectsAllWhitespacePrefixesWithoutSideEffects) {
  for (const auto prefix : {" ", "\t", "\r", "\n"}) {
    int callback_count = 0;
    const auto result = cache->List(prefix, [&](std::string_view, const sitos::ParamValue&) {
      ++callback_count;
      return true;
    });
    EXPECT_EQ(result.StatusCode(), sitos::Status::InvalidKey);
    EXPECT_EQ(callback_count, 0);
    EXPECT_EQ(cache->Get<std::int64_t>("a").Value(), 1);
  }

  ASSERT_TRUE(cache->Put("t", std::int64_t{2}).IsOk());
  int callback_count = 0;
  EXPECT_TRUE(cache->List("t", [&](std::string_view, const sitos::ParamValue&) {
    ++callback_count;
    return true;
  }).IsOk());
  EXPECT_EQ(callback_count, 1);
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

TEST_F(ParamCacheReadTest, SpanHandleValidatesTypeLengthAndEmptyBytes) {
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{1}, std::byte{2}}).IsOk());
  EXPECT_EQ(cache->GetSpan<std::uint32_t>("bytes").StatusCode(), sitos::Status::TypeMismatch);
  auto handle = cache->GetSpan<std::byte>("bytes");
  ASSERT_TRUE(handle.IsOk());
  ASSERT_EQ(handle.Value().span.size(), 2U);
  EXPECT_EQ(handle.Value().span[0], std::byte{1});

  ASSERT_TRUE(cache->Put("empty", std::vector<std::byte>{}).IsOk());
  auto empty = cache->GetSpan<std::byte>("empty");
  ASSERT_TRUE(empty.IsOk());
  EXPECT_TRUE(empty.Value().span.empty());
}

TEST_F(ParamCacheReadTest, SpanHandleSurvivesOverwriteDetachMoveAndCacheDestruction) {
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{1}, std::byte{2}}).IsOk());
  auto handle = cache->GetSpan<std::byte>("bytes");
  ASSERT_TRUE(handle.IsOk());
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{3}}).IsOk());
  cache->Detach();
  EXPECT_EQ(handle.Value().span[0], std::byte{1});

  auto opened = sitos::ParamCache::Open(transport);
  ASSERT_TRUE(opened.IsOk());
  auto moved = std::move(opened).Value();
  ASSERT_TRUE(moved.Attach("s1").IsOk());
  ASSERT_TRUE(moved.Put("bytes", std::vector<std::byte>{std::byte{4}}).IsOk());
  auto moved_handle = moved.GetSpan<std::byte>("bytes");
  ASSERT_TRUE(moved_handle.IsOk());
  sitos::ParamCache destination = std::move(moved);
  EXPECT_EQ(destination.GetSpan<std::byte>("bytes").Value().span[0], std::byte{4});
  destination.Detach();
  EXPECT_EQ(moved_handle.Value().span[0], std::byte{4});

  std::optional<sitos::SpanHandle<std::byte>> destroyed_handle;
  {
    auto temporary_opened = sitos::ParamCache::Open(transport);
    ASSERT_TRUE(temporary_opened.IsOk());
    auto temporary = std::move(temporary_opened).Value();
    ASSERT_TRUE(temporary.Attach("s1").IsOk());
    ASSERT_TRUE(temporary.Put("owned", std::vector<std::byte>{std::byte{5}}).IsOk());
    auto temporary_handle = temporary.GetSpan<std::byte>("owned");
    ASSERT_TRUE(temporary_handle.IsOk());
    destroyed_handle.emplace(std::move(temporary_handle).Value());
  }
  ASSERT_TRUE(destroyed_handle.has_value());
  EXPECT_EQ(destroyed_handle->span[0], std::byte{5});
}

TEST_F(ParamCacheReadTest, RejectedWritesAndTransportFailuresPreserveLocalState) {
  const auto before = transport->put_count;
  EXPECT_EQ(cache->Put("bad/key/", std::int64_t{9}).StatusCode(), sitos::Status::InvalidKey);
  const std::vector<sitos::BatchEntry> invalid_entries{{"valid", sitos::ParamValue(2)},
                                                         {"bad/key/", sitos::ParamValue(3)}};
  EXPECT_EQ(cache->PutBatch(invalid_entries).StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(transport->put_count, before);

  const auto cause = std::make_error_code(std::errc::io_error);
  transport->put_result = sitos::Result<void>::Err(sitos::Status::Disconnected, "offline", cause);
  const auto put_result = cache->Put("a", std::int64_t{9});
  EXPECT_EQ(put_result.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(put_result.Message(), "offline");
  EXPECT_EQ(put_result.Error(), cause);
  const std::vector<sitos::BatchEntry> entries{{"a", sitos::ParamValue(9)}};
  const auto batch_result = cache->PutBatch(entries);
  EXPECT_EQ(batch_result.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(batch_result.Message(), "offline");
  EXPECT_EQ(batch_result.Error(), cause);
  EXPECT_EQ(cache->Get<std::int64_t>("a").Value(), 1);
}

TEST_F(ParamCacheReadTest, SynchronousSubscriberCallbackDoesNotDeadlockPut) {
  transport->sync_delivery = true;
  ASSERT_TRUE(cache->Put("sync", std::int64_t{8}).IsOk());
  EXPECT_EQ(cache->Get<std::int64_t>("sync").Value(), 8);
  EXPECT_EQ(transport->put_count, 1U);
}

TEST_F(ParamCacheReadTest, DelayedSelfEchoUsesSubscriberSerializationOrder) {
  ASSERT_TRUE(cache->Put("ordered", std::int64_t{1}).IsOk());
  ASSERT_TRUE(cache->Put("ordered", std::int64_t{2}).IsOk());
  ASSERT_EQ(transport->put_payloads.size(), 2U);
  ASSERT_TRUE(transport->subscriber);
  const auto& payload = transport->put_payloads.front();
  sitos::TransportSample delayed{"sitos/session/s1/ordered", payload,
                                sitos::Encoding{std::string(sitos::Encoding::kSitosV1)},
                                std::nullopt, sitos::TransportSample::Kind::Put};
  transport->subscriber(delayed);
  EXPECT_EQ(cache->Get<std::int64_t>("ordered").Value(), 1);
}

TEST_F(ParamCacheReadTest, PutBatchUsesOneCanonicalSubmissionAndPreservesOrder) {
  const std::vector<sitos::BatchEntry> entries{{"b", sitos::ParamValue(2)},
                                                {"a", sitos::ParamValue(1)},
                                                {"b", sitos::ParamValue(3)}};
  ASSERT_TRUE(cache->PutBatch(entries).IsOk());
  ASSERT_EQ(transport->puts.size(), 1U);
  EXPECT_NE(transport->puts.front().find("/session/s1/:batch"), std::string::npos);
  ASSERT_EQ(transport->put_encodings.front(), sitos::Encoding::kSitosV1Batch);
  ASSERT_FALSE(transport->put_options.front().ack);
  const auto decoded = sitos::DecodeBatch(transport->put_payloads.front());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), entries.size());
  EXPECT_EQ((*decoded)[0].key, "b");
  EXPECT_EQ((*decoded)[1].key, "a");
  EXPECT_EQ((*decoded)[2].key, "b");
  EXPECT_EQ((*decoded)[2].value.As<std::int64_t>(), 3);
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
  ASSERT_TRUE(cache->Put("foo/a", 4).IsOk());
  ASSERT_TRUE(cache->Put("foobar", 2).IsOk());
  ASSERT_TRUE(cache->Put("foo", 1).IsOk());
  std::vector<std::string> keys;
  ASSERT_TRUE(cache->List("foo", [&](std::string_view key, const sitos::ParamValue&) {
    keys.emplace_back(key);
    return true;
  }).IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo", "foo/a", "foo/z", "foobar"}));
  keys.clear();
  ASSERT_TRUE(cache->List("foo/", [&](std::string_view key, const sitos::ParamValue&) {
    keys.emplace_back(key);
    return true;
  }).IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/a", "foo/z"}));
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
  const auto before_puts = transport->put_count;
  const auto before_gets = transport->get_count;
  EXPECT_TRUE(cache->GetShared("a").IsOk());
  EXPECT_TRUE(cache->Get<std::int64_t>("a").IsOk());
  EXPECT_TRUE(cache->GetOr<std::int64_t>("a", 9).IsOk());
  EXPECT_TRUE(cache->GetSpan<std::byte>("a").StatusCode() == sitos::Status::TypeMismatch);
  EXPECT_TRUE(cache->Contains("a").IsOk());
  EXPECT_TRUE(cache->List("a", [](std::string_view, const sitos::ParamValue&) { return true; }).IsOk());
  EXPECT_EQ(transport->put_count, before_puts);
  EXPECT_EQ(transport->get_count, before_gets);
}

TEST_F(ParamCacheReadTest, ListPropagatesSinkExceptionAndUsesStableSnapshot) {
  ASSERT_TRUE(cache->Put("foo/z", std::int64_t{3}).IsOk());
  ASSERT_TRUE(cache->Put("foo/a", std::int64_t{1}).IsOk());
  std::vector<std::string> keys;
  sitos::ListSink sink = [&](std::string_view key, const sitos::ParamValue&) -> bool {
    keys.emplace_back(key);
    throw std::runtime_error("sink failure");
  };
  EXPECT_THROW(cache->List("foo/", sink), std::runtime_error);
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/a"}));

  keys.clear();
  const auto caller = std::this_thread::get_id();
  ASSERT_TRUE(cache->List("foo/", [&](std::string_view key, const sitos::ParamValue&) {
    EXPECT_EQ(std::this_thread::get_id(), caller);
    keys.emplace_back(key);
    if (key == "foo/a") {
      EXPECT_TRUE(cache->Put("foo/new", std::int64_t{4}).IsOk());
    }
    return true;
  }).IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/a", "foo/z"}));
  EXPECT_EQ(cache->Get<std::int64_t>("foo/new").Value(), 4);
}

TEST_F(ParamCacheReadTest, DetachWaitsForInFlightLocalOperation) {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  bool detached = false;
  Access::SetMutationHook(*cache, [&](std::size_t) {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  });

  std::thread put_thread([&] { ASSERT_TRUE(cache->Put("lease", std::int64_t{4}).IsOk()); });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  std::thread detach_thread([&] {
    cache->Detach();
    std::lock_guard lock(mutex);
    detached = true;
    condition.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    EXPECT_FALSE(condition.wait_for(lock, std::chrono::milliseconds(50), [&] { return detached; }));
    release = true;
    condition.notify_all();
  }
  put_thread.join();
  detach_thread.join();
  EXPECT_TRUE(detached);
  EXPECT_FALSE(Access::IsAttached(*cache));
}

}  // namespace
