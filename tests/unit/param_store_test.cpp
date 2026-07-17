// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"

namespace {

using sitos::BatchEntry;
using sitos::ClientConfig;
using sitos::Encoding;
using sitos::ParamValue;
using sitos::PutOptions;
using sitos::Queryable;
using sitos::Result;
using sitos::Subscription;
using sitos::Transport;
using sitos::TransportQuery;
using sitos::TransportSample;

class FakeTransport final : public Transport {
 public:
  struct PutRecord {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
  };

  struct ReplyRecord {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
  };

  Result<void> Put(std::string_view key, std::span<const std::byte> payload, Encoding encoding,
                   PutOptions) override {
    std::lock_guard lock(mutex_);
    puts.push_back({std::string(key), {payload.begin(), payload.end()}, std::move(encoding)});
    return put_result;
  }

  Result<void> Delete(std::string_view key, PutOptions) override {
    std::lock_guard lock(mutex_);
    deletes.emplace_back(key);
    return delete_result;
  }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds) override {
    std::vector<ReplyRecord> reply_copy;
    Result<void> result = Result<void>::Ok();
    {
      std::lock_guard lock(mutex_);
      get_keys.emplace_back(keyexpr);
      reply_copy = replies;
      result = get_result;
    }
    const auto invoke = [&] {
      {
        std::lock_guard lock(mutex_);
        callback_thread_id = std::this_thread::get_id();
      }
      for (const auto& reply : reply_copy) {
        if (!sink(reply.key, reply.payload, reply.encoding)) break;
      }
    };
    if (invoke_get_on_worker) {
      std::thread worker(invoke);
      worker.join();
    } else {
      invoke();
    }
    {
      std::lock_guard lock(mutex_);
      get_returned = true;
    }
    return result;
  }

  Result<Subscription> DeclareSubscriber(std::string_view,
                                         std::function<void(const TransportSample&)>) override {
    return Result<Subscription>::Ok(Subscription{});
  }

  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)>) override {
    return Result<Queryable>::Ok(Queryable{});
  }

  std::vector<PutRecord> puts;
  std::vector<std::string> deletes;
  std::vector<std::string> get_keys;
  std::vector<ReplyRecord> replies;
  Result<void> put_result = Result<void>::Ok();
  Result<void> delete_result = Result<void>::Ok();
  Result<void> get_result = Result<void>::Ok();
  bool invoke_get_on_worker = false;
  bool get_returned = false;
  std::thread::id callback_thread_id;

 private:
  std::mutex mutex_;
};

std::shared_ptr<FakeTransport> OpenFake() {
  auto transport = std::make_shared<FakeTransport>();
  auto result = sitos::ParamStore::Open(transport);
  EXPECT_TRUE(result.IsOk()) << result.Message();
  return transport;
}

TEST(ParamStoreTest, OpenRejectsNullTransportAndInapplicableJsonBeforeUse) {
  auto null_result = sitos::ParamStore::Open(std::shared_ptr<Transport>{});
  ASSERT_FALSE(null_result.IsOk());
  EXPECT_EQ(null_result.StatusCode(), sitos::Status::InvalidArgument);

  auto transport = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.zenoh_config_json = "{mode: 'peer'}";
  auto json_result = sitos::ParamStore::Open(transport, std::move(config));
  ASSERT_FALSE(json_result.IsOk());
  EXPECT_EQ(json_result.StatusCode(), sitos::Status::InvalidArgument);

  ClientConfig invalid_prefix;
  invalid_prefix.prefix = "";
  auto prefix_result = sitos::ParamStore::Open(transport, std::move(invalid_prefix));
  ASSERT_FALSE(prefix_result.IsOk());
  EXPECT_EQ(prefix_result.StatusCode(), sitos::Status::InvalidKey);

  ClientConfig invalid_timeout;
  invalid_timeout.query_timeout = std::chrono::milliseconds::zero();
  auto timeout_result = sitos::ParamStore::Open(transport, std::move(invalid_timeout));
  ASSERT_FALSE(timeout_result.IsOk());
  EXPECT_EQ(timeout_result.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_TRUE(transport->puts.empty());
  EXPECT_TRUE(transport->get_keys.empty());
  EXPECT_TRUE(transport->deletes.empty());
}

TEST(ParamStoreTest, RejectsInvalidWritesWithoutTransportOperations) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  EXPECT_EQ(store.Put("invalid", "key", ParamValue(1)).StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(store.Put("snap/session1", "key", ParamValue(1)).StatusCode(), sitos::Status::ReadOnly);
  EXPECT_EQ(store.Delete("session/session1", "key").StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(store.Delete("snap/session1", "key").StatusCode(), sitos::Status::ReadOnly);
  EXPECT_TRUE(transport->puts.empty());
  EXPECT_TRUE(transport->deletes.empty());
}

TEST(ParamStoreTest, TypedPutRejectsNullStringsAndOutOfRangeIntegersBeforeWire) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  const char* null_string = nullptr;
  EXPECT_EQ(store.Put("base", "key", null_string).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(store.Put("base", "key", std::numeric_limits<std::uint64_t>::max()).StatusCode(),
            sitos::Status::InvalidArgument);
  EXPECT_TRUE(transport->puts.empty());
}

TEST(ParamStoreTest, PutUsesCanonicalPayloadAndBatchIsOneWireMessage) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  ASSERT_TRUE(store.Put("base", "foo/bar", std::int64_t{7}).IsOk());
  ASSERT_EQ(transport->puts.size(), 1U);
  EXPECT_EQ(transport->puts[0].key, "sitos/base/foo/bar");
  EXPECT_EQ(transport->puts[0].encoding.id, Encoding::kSitosV1);
  ASSERT_EQ(transport->puts[0].payload.size(), 9U);

  const std::vector<BatchEntry> entries = {{"foo", ParamValue(1)}, {"foo", ParamValue(2)}};
  ASSERT_TRUE(store.PutBatch("base", {}).IsOk());
  EXPECT_EQ(transport->puts.size(), 1U);
  EXPECT_EQ(store.PutBatch("invalid", {}).StatusCode(), sitos::Status::InvalidKey);
  ASSERT_TRUE(store.PutBatch("session/s1", entries).IsOk());
  ASSERT_EQ(transport->puts.size(), 2U);
  EXPECT_EQ(transport->puts[1].key, "sitos/session/s1/:batch");
  EXPECT_EQ(transport->puts[1].encoding.id, Encoding::kSitosV1Batch);
  auto decoded = sitos::DecodeBatch(transport->puts[1].payload);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), 2U);
  EXPECT_EQ((*decoded)[0].key, "foo");
  EXPECT_EQ((*decoded)[1].value.As<std::int64_t>(), 2);
}

TEST(ParamStoreTest, ExactGetAndContainsMapZeroReplyAndDecodeErrors) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  auto missing = store.Get("base", "missing");
  ASSERT_FALSE(missing.IsOk());
  EXPECT_EQ(missing.StatusCode(), sitos::Status::NotFound);
  auto contains = store.Contains("base", "missing");
  ASSERT_TRUE(contains.IsOk());
  EXPECT_FALSE(contains.Value());

  transport->replies.push_back(
      {"sitos/base/key", {std::byte{0xff}}, Encoding{std::string(Encoding::kSitosV1)}});
  auto malformed = store.Get("base", "key");
  ASSERT_FALSE(malformed.IsOk());
  EXPECT_EQ(malformed.StatusCode(), sitos::Status::Error);
}

TEST(ParamStoreTest, RejectsUnexpectedExactRepliesAndContainsPreservesErrors) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  transport->replies = {{"sitos/base/other", ParamValue(1).Encode(),
                         Encoding{std::string(Encoding::kSitosV1)}}};
  auto mismatched = store.Get("base", "key");
  ASSERT_FALSE(mismatched.IsOk());
  EXPECT_EQ(mismatched.StatusCode(), sitos::Status::Error);

  transport->replies = {{"sitos/base/key", ParamValue(1).Encode(),
                         Encoding{"application/octet-stream"}}};
  auto wrong_encoding = store.Get("base", "key");
  ASSERT_FALSE(wrong_encoding.IsOk());
  EXPECT_EQ(wrong_encoding.StatusCode(), sitos::Status::Error);

  auto contains = store.Contains("base", "key");
  ASSERT_FALSE(contains.IsOk());
  EXPECT_EQ(contains.StatusCode(), sitos::Status::Error);
}

TEST(ParamStoreTest, ListRejectsInvalidMatchingReplies) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  const std::vector<FakeTransport::ReplyRecord> invalid_replies = {
      {"sitos/base/foo/key", ParamValue(1).Encode(), Encoding{"application/octet-stream"}},
      {"sitos/base/foo/key", {std::byte{0xff}},
       Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/session/s1/foo/key", ParamValue(1).Encode(),
       Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/base/:batch", ParamValue(1).Encode(),
       Encoding{std::string(Encoding::kSitosV1)}},
  };
  for (const auto& reply : invalid_replies) {
    transport->replies = {reply};
    auto result = store.List("base", "foo", [](std::string_view, const ParamValue&) {
      ADD_FAILURE() << "invalid reply reached the ListSink";
      return true;
    });
    EXPECT_EQ(result.StatusCode(), sitos::Status::Error);
  }
}

TEST(ParamStoreTest, ListSortsRepliesFiltersPrefixAndStopsOnFalse) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  transport->replies = {
      {"sitos/base/foo/z", ParamValue(2).Encode(), Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/base/foo/a", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}},
  };
  std::vector<std::string> keys;
  ASSERT_TRUE(store
                  .List("base", "foo/",
                        [&](std::string_view key, const ParamValue&) {
                          keys.emplace_back(key);
                          return true;
                        })
                  .IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/a", "foo/z"}));

  transport->replies.push_back(
      {"sitos/base/foobar", ParamValue(3).Encode(), Encoding{std::string(Encoding::kSitosV1)}});
  keys.clear();
  ASSERT_TRUE(store
                  .List("base", "foo",
                        [&](std::string_view key, const ParamValue&) {
                          keys.emplace_back(key);
                          return false;
                        })
                  .IsOk());
  ASSERT_EQ(keys.size(), 1U);
  EXPECT_EQ(keys[0], "foo/a");
}

TEST(ParamStoreTest, RejectsInvalidListPrefixesWithoutTransportOperations) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  for (const std::string_view prefix :
       {"/foo", "foo//bar", "foo/*", "foo/:batch", "foo bar", "/"}) {
    EXPECT_EQ(store.List("base", prefix, [](std::string_view, const ParamValue&) { return true; })
                  .StatusCode(),
              sitos::Status::InvalidKey);
  }
  EXPECT_TRUE(transport->get_keys.empty());
}

TEST(ParamStoreTest, PreservesTransportErrorsAndTypedConversionStatus) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  const auto cause = std::make_error_code(std::errc::connection_reset);
  transport->get_result = Result<void>::Err(sitos::Status::Disconnected, "offline", cause);
  auto disconnected = store.Get("base", "key");
  ASSERT_FALSE(disconnected.IsOk());
  EXPECT_EQ(disconnected.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(disconnected.Message(), "offline");
  EXPECT_EQ(disconnected.Error(), cause);

  transport->get_result = Result<void>::Ok();
  transport->replies = {
      {"sitos/base/key", ParamValue("text").Encode(), Encoding{std::string(Encoding::kSitosV1)}}};
  auto mismatch = store.Get<std::int64_t>("base", "key");
  ASSERT_FALSE(mismatch.IsOk());
  EXPECT_EQ(mismatch.StatusCode(), sitos::Status::TypeMismatch);
}

TEST(ParamStoreTest, ListIgnoresUnrelatedRepliesInsideSafeParentBeforeDecoding) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  transport->replies = {
      {"sitos/base/foo/bar/value", ParamValue(1).Encode(),
       Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/base/foo/unrelated", {std::byte{0xff}}, Encoding{"application/octet-stream"}},
  };
  std::vector<std::string> keys;
  ASSERT_TRUE(store
                  .List("base", "foo/bar",
                        [&](std::string_view key, const ParamValue&) {
                          keys.emplace_back(key);
                          return true;
                        })
                  .IsOk());
  EXPECT_EQ(keys, (std::vector<std::string>{"foo/bar/value"}));
}

TEST(ParamStoreTest, ListRunsSinkOnCallerThreadAfterTransportCallback) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  transport->invoke_get_on_worker = true;
  transport->replies = {
      {"sitos/base/key", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}}};

  const auto caller_thread_id = std::this_thread::get_id();
  std::thread::id sink_thread_id;
  bool callback_had_returned = false;
  ASSERT_TRUE(store
                  .List("base", "", [&](std::string_view, const ParamValue&) {
                    sink_thread_id = std::this_thread::get_id();
                    callback_had_returned = transport->get_returned;
                    return true;
                  })
                  .IsOk());
  EXPECT_EQ(sink_thread_id, caller_thread_id);
  EXPECT_NE(transport->callback_thread_id, caller_thread_id);
  EXPECT_TRUE(callback_had_returned);
}

TEST(ParamStoreTest, IndependentParamStoreCallsMakeProgressConcurrently) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  transport->replies = {
      {"sitos/base/key", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}}};

  std::atomic<bool> failed = false;
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&, i] {
      if (!store.Put("base", "key", static_cast<std::int64_t>(i)).IsOk() ||
          !store.Get("base", "key").IsOk()) {
        failed.store(true);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  EXPECT_FALSE(failed.load());
  EXPECT_EQ(transport->puts.size(), 8U);
}

TEST(ParamStoreTest, ListSinkExceptionPropagatesAndStoreRemainsUsable) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  transport->replies = {
      {"sitos/base/key", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}}};

  EXPECT_THROW(store.List("base", "",
                          [](std::string_view, const ParamValue&) -> bool {
                            throw std::runtime_error("sink failure");
                          }),
               std::runtime_error);
  transport->replies.clear();
  auto contains = store.Contains("base", "missing");
  ASSERT_TRUE(contains.IsOk());
  EXPECT_FALSE(contains.Value());
}

}  // namespace
