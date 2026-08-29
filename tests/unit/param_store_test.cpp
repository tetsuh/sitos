// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/batch.hpp"

namespace {

using namespace std::chrono_literals;
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
                   PutOptions options) override {
    std::lock_guard lock(mutex_);
    puts.push_back({std::string(key), {payload.begin(), payload.end()}, std::move(encoding)});
    last_put_options = std::move(options);
    if (!put_results.empty()) {
      auto result = std::move(put_results.front());
      put_results.pop_front();
      return result;
    }
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
      if (keyexpr.find("/meta/ack/") != std::string_view::npos) {
        if (!ack_payloads.empty()) {
          auto scripted = std::move(ack_payloads.front());
          ack_payloads.pop_front();
          if (scripted.has_value()) {
            if (scripted->key.empty()) scripted->key = std::string(keyexpr);
            reply_copy = {std::move(*scripted)};
          } else
            reply_copy.clear();
        } else if (ack_reply.has_value()) {
          auto encoded = sitos::EncodeAckResult(*ack_reply);
          if (encoded.IsOk()) {
            reply_copy = {{std::string(keyexpr), std::move(encoded).Value(),
                           Encoding{std::string(Encoding::kSitosV1Ack)}}};
          }
        }
        for (auto& reply : reply_copy) {
          if (reply.key.empty()) reply.key = std::string(keyexpr);
        }
      }
      if (!get_results.empty()) {
        result = std::move(get_results.front());
        get_results.pop_front();
      } else {
        result = get_result;
      }
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
  std::optional<sitos::AckResultV1> ack_reply;
  std::deque<std::optional<ReplyRecord>> ack_payloads;
  std::deque<Result<void>> put_results;
  std::deque<Result<void>> get_results;
  std::optional<PutOptions> last_put_options;
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

FakeTransport::ReplyRecord EncodeAckReply(const sitos::AckResultV1& result) {
  auto encoded = sitos::EncodeAckResult(result);
  EXPECT_TRUE(encoded.IsOk()) << encoded.Message();
  return {"", std::move(encoded).Value(), Encoding{std::string(Encoding::kSitosV1Ack)}};
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

  ASSERT_TRUE(
      store.Put("base", "foo/bar", std::int64_t{7}, sitos::ParamStore::WriteOptions{.ack = false})
          .IsOk());
  ASSERT_EQ(transport->puts.size(), 1U);
  EXPECT_EQ(transport->puts[0].key, "sitos/base/foo/bar");
  EXPECT_EQ(transport->puts[0].encoding.id, Encoding::kSitosV1);
  ASSERT_EQ(transport->puts[0].payload.size(), 9U);

  const std::vector<BatchEntry> entries = {{"foo", ParamValue(1)}, {"foo", ParamValue(2)}};
  ASSERT_TRUE(store.PutBatch("base", {}, sitos::ParamStore::WriteOptions{.ack = false}).IsOk());
  EXPECT_EQ(transport->puts.size(), 1U);
  EXPECT_EQ(store.PutBatch("invalid", {}).StatusCode(), sitos::Status::InvalidKey);
  ASSERT_TRUE(
      store.PutBatch("session/s1", entries, sitos::ParamStore::WriteOptions{.ack = false}).IsOk());
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

  transport->replies = {
      {"sitos/base/other", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}}};
  auto mismatched = store.Get("base", "key");
  ASSERT_FALSE(mismatched.IsOk());
  EXPECT_EQ(mismatched.StatusCode(), sitos::Status::Error);

  transport->replies = {
      {"sitos/base/key", ParamValue(1).Encode(), Encoding{"application/octet-stream"}}};
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
      {"sitos/base/foo/key", {std::byte{0xff}}, Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/session/s1/foo/key", ParamValue(1).Encode(),
       Encoding{std::string(Encoding::kSitosV1)}},
      {"sitos/base/:batch", ParamValue(1).Encode(), Encoding{std::string(Encoding::kSitosV1)}},
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
      {"sitos/base/foo", {std::byte{0xff}}, Encoding{"application/octet-stream"}},
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
                  .List("base", "",
                        [&](std::string_view, const ParamValue&) {
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
      if (!store
               .Put("base", "key", static_cast<std::int64_t>(i),
                    sitos::ParamStore::WriteOptions{.ack = false})
               .IsOk() ||
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

TEST(ParamStoreAckTest, ValidatesOptionsBeforeSubmission) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  const char* null_string = nullptr;
  const std::vector<BatchEntry> invalid_batch = {{"", ParamValue(1)}};
  EXPECT_EQ(store
                .Put("base", "key", ParamValue(1),
                     sitos::ParamStore::WriteOptions{.ack = true, .ack_timeout = 0ms})
                .StatusCode(),
            sitos::Status::InvalidArgument);
  EXPECT_EQ(store.Put("snap/s1", "key", ParamValue(1)).StatusCode(), sitos::Status::ReadOnly);
  EXPECT_EQ(store.Put("base", "key", null_string).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(store.Put("base", "bad key", ParamValue(1)).StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(store.PutBatch("base", invalid_batch).StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(store
                .Put("invalid", "key", ParamValue(1),
                     sitos::ParamStore::WriteOptions{.ack = false, .ack_timeout = 0ms})
                .StatusCode(),
            sitos::Status::InvalidKey);
  EXPECT_EQ(
      store
          .PutBatch("snap/s1", {}, sitos::ParamStore::WriteOptions{.ack = true, .ack_timeout = 0ms})
          .StatusCode(),
      sitos::Status::ReadOnly);
  EXPECT_TRUE(transport->puts.empty());
  EXPECT_TRUE(transport->get_keys.empty());

  auto moved = std::move(store);
  auto moved_from = store.Put("base", "key", ParamValue(1));
  EXPECT_EQ(moved_from.StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_TRUE(transport->puts.empty());
  EXPECT_TRUE(transport->get_keys.empty());
}

TEST(ParamStoreAckTest, SubmitsOnceAndMapsResult) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();

  const auto make_put_ack = [](sitos::Status status, std::string message) {
    const bool ok = status == sitos::Status::Ok;
    return sitos::AckResultV1{sitos::AckOperationKind::Put,       status,
                              sitos::AckDurability::Applied,      ok ? 1U : 0U,
                              ok ? sitos::kAckNoFailedIndex : 0U, 0,
                              sitos::kAckNoFailedSequence,        std::move(message)};
  };
  transport->ack_reply = make_put_ack(sitos::Status::Ok, "stored");
  auto result = store.Put("base", "key", ParamValue(1));
  ASSERT_TRUE(result.IsOk()) << result.Message();
  ASSERT_EQ(transport->puts.size(), 1U);
  ASSERT_TRUE(transport->last_put_options->ack_token.has_value());
  ASSERT_EQ(transport->get_keys.size(), 1U);

  const std::vector<sitos::Status> statuses = {
      sitos::Status::NotFound, sitos::Status::TypeMismatch,  sitos::Status::Disconnected,
      sitos::Status::ReadOnly, sitos::Status::InvalidKey,    sitos::Status::InvalidArgument,
      sitos::Status::Error,    sitos::Status::OutcomeUnknown};
  for (const auto status : statuses) {
    transport->ack_reply = make_put_ack(status, "remote diagnostic");
    const auto mapped = store.Put("base", "status", ParamValue(2),
                                  sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
    ASSERT_FALSE(mapped.IsOk());
    EXPECT_EQ(mapped.StatusCode(), status);
    EXPECT_EQ(mapped.Message(), "remote diagnostic");
  }

  transport->ack_reply = sitos::AckResultV1{sitos::AckOperationKind::Batch, sitos::Status::Ok,
                                            sitos::AckDurability::Applied,  2,
                                            sitos::kAckNoFailedIndex,       0,
                                            sitos::kAckNoFailedSequence,    "batch"};
  const std::vector<BatchEntry> entries = {{"a", ParamValue(1)}, {"b", ParamValue(2)}};
  auto default_batch = store.PutBatch("base", entries);
  ASSERT_TRUE(default_batch.IsOk()) << default_batch.Message();
  EXPECT_EQ(transport->puts.back().encoding.id, Encoding::kSitosV1Batch);
  EXPECT_TRUE(transport->last_put_options->ack_token.has_value());

  auto batch =
      store.PutBatch("base", entries, sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_TRUE(batch.IsOk()) << batch.Message();
  EXPECT_EQ(transport->puts.back().encoding.id, Encoding::kSitosV1Batch);
  EXPECT_TRUE(transport->last_put_options->ack_token.has_value());

  transport->ack_reply = sitos::AckResultV1{sitos::AckOperationKind::Batch,
                                            sitos::Status::OutcomeUnknown,
                                            sitos::AckDurability::Applied,
                                            1,
                                            1,
                                            0,
                                            sitos::kAckNoFailedSequence,
                                            "batch partially applied"};
  auto partial_batch = store.PutBatch("base", entries);
  ASSERT_FALSE(partial_batch.IsOk());
  EXPECT_EQ(partial_batch.StatusCode(), sitos::Status::OutcomeUnknown);
  EXPECT_EQ(partial_batch.Message(), "batch partially applied");

  transport->ack_reply = sitos::AckResultV1{sitos::AckOperationKind::Batch, sitos::Status::Ok,
                                            sitos::AckDurability::Applied,  2,
                                            sitos::kAckNoFailedIndex,       0,
                                            sitos::kAckNoFailedSequence,    "wrong operation"};
  auto mismatch = store.Put("base", "mismatch", ParamValue(3),
                            sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_FALSE(mismatch.IsOk());
  EXPECT_EQ(mismatch.StatusCode(), sitos::Status::Error);

  transport->ack_reply.reset();
  transport->replies = {EncodeAckReply(make_put_ack(sitos::Status::Error, "wrong key"))};
  transport->replies.front().key = "sitos/meta/ack/wrong";
  auto wrong_key = store.Put("base", "wrong-key", ParamValue(4),
                             sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_EQ(wrong_key.StatusCode(), sitos::Status::Error);
  EXPECT_EQ(wrong_key.Message(), "ack protocol error: reply key differs from query");

  transport->replies = {EncodeAckReply(make_put_ack(sitos::Status::Error, "wrong encoding"))};
  transport->replies.front().encoding = Encoding{"application/octet-stream"};
  auto wrong_encoding = store.Put("base", "wrong-encoding", ParamValue(4),
                                  sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_EQ(wrong_encoding.StatusCode(), sitos::Status::Error);
  EXPECT_EQ(wrong_encoding.Message(), "ack protocol error: reply Encoding is not sitos.v1.ack");

  transport->replies = {{"", {std::byte{0xff}}, Encoding{std::string(Encoding::kSitosV1Ack)}}};
  auto malformed = store.Put("base", "malformed", ParamValue(4),
                             sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_EQ(malformed.StatusCode(), sitos::Status::Error);
  EXPECT_EQ(malformed.Message(), "ack protocol error: malformed AckResultV1");

  auto raw_timeout = EncodeAckReply(make_put_ack(sitos::Status::Error, "wire timeout"));
  raw_timeout.payload[2] = static_cast<std::byte>(sitos::Status::Timeout);
  transport->replies = {std::move(raw_timeout)};
  auto remote_timeout = store.Put("base", "remote-timeout", ParamValue(4),
                                  sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  ASSERT_EQ(remote_timeout.StatusCode(), sitos::Status::Error);
  EXPECT_EQ(remote_timeout.Message(), "ack protocol error: malformed AckResultV1");

  transport->replies.clear();
  transport->put_results.push_back(Result<void>::Err(sitos::Status::Disconnected, "put failed"));
  transport->ack_reply = make_put_ack(sitos::Status::Ok, "recovered");
  auto put_recovered = store.Put("base", "put-recovered", ParamValue(5),
                                 sitos::ParamStore::WriteOptions{.ack_timeout = 20ms});
  EXPECT_TRUE(put_recovered.IsOk()) << put_recovered.Message();

  const auto recovered_puts = transport->puts.size();
  const auto recovered_gets = transport->get_keys.size();
  transport->ack_reply.reset();
  transport->get_results.push_back(
      Result<void>::Err(sitos::Status::Disconnected, "first get failed"));
  transport->ack_payloads.push_back(std::nullopt);
  auto get_ack = EncodeAckReply(make_put_ack(sitos::Status::Ok, "get recovered"));
  transport->ack_payloads.push_back(std::move(get_ack));
  auto get_recovered = store.Put("base", "get-recovered", ParamValue(6),
                                 sitos::ParamStore::WriteOptions{.ack_timeout = 250ms});
  ASSERT_TRUE(get_recovered.IsOk()) << get_recovered.Message();
  ASSERT_EQ(transport->puts.size(), recovered_puts + 1);
  ASSERT_GE(transport->get_keys.size(), recovered_gets + 2);
  for (std::size_t i = recovered_gets; i < transport->get_keys.size(); ++i) {
    EXPECT_EQ(transport->get_keys[i], transport->get_keys[recovered_gets]);
  }
  ASSERT_TRUE(transport->last_put_options->ack_token.has_value());
  const auto recovered_token = sitos::FormatAckToken(*transport->last_put_options->ack_token);
  EXPECT_NE(transport->get_keys[recovered_gets].find("/meta/ack/" + recovered_token),
            std::string::npos);

  transport->ack_reply.reset();
  const auto timeout_puts = transport->puts.size();
  const auto timeout_gets = transport->get_keys.size();
  transport->get_results.push_back(
      Result<void>::Err(sitos::Status::Disconnected, "latest native cause",
                        std::make_error_code(std::errc::connection_reset)));
  auto timed_out = store.Put("base", "timeout", ParamValue(7),
                             sitos::ParamStore::WriteOptions{.ack_timeout = 250ms});
  ASSERT_FALSE(timed_out.IsOk());
  EXPECT_EQ(timed_out.StatusCode(), sitos::Status::Timeout);
  EXPECT_NE(timed_out.Message().find("latest native cause"), std::string::npos);
  EXPECT_EQ(timed_out.Error(), std::make_error_code(std::errc::connection_reset));
  EXPECT_EQ(transport->puts.size(), timeout_puts + 1);
  EXPECT_GE(transport->get_keys.size(), timeout_gets + 2);
}

TEST(ParamStoreAckTest, PreservesSubmissionOnlyWrites) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  const auto cause = std::make_error_code(std::errc::connection_reset);
  auto success = store.Put("base", "success", ParamValue(1),
                           sitos::ParamStore::WriteOptions{
                               .ack = false, .ack_timeout = std::chrono::milliseconds::max()});
  ASSERT_TRUE(success.IsOk());
  EXPECT_FALSE(transport->last_put_options->ack_token.has_value());

  const std::vector<BatchEntry> entries = {{"a", ParamValue(1)}, {"b", ParamValue(2)}};
  auto batch_success = store.PutBatch(
      "base", entries, sitos::ParamStore::WriteOptions{.ack = false, .ack_timeout = -1ms});
  ASSERT_TRUE(batch_success.IsOk());
  EXPECT_FALSE(transport->last_put_options->ack_token.has_value());

  transport->put_result = Result<void>::Err(sitos::Status::Disconnected, "offline", cause);
  auto result = store.Put("base", "key", ParamValue(1),
                          sitos::ParamStore::WriteOptions{.ack = false, .ack_timeout = 0ms});
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(result.Message(), "offline");
  EXPECT_EQ(result.Error(), cause);
  auto batch_failure =
      store.PutBatch("base", entries, sitos::ParamStore::WriteOptions{.ack = false});
  ASSERT_FALSE(batch_failure.IsOk());
  EXPECT_EQ(batch_failure.StatusCode(), sitos::Status::Disconnected);
  EXPECT_EQ(transport->get_keys.size(), 0U);
  EXPECT_EQ(transport->puts.size(), 4U);
}

TEST(ParamStoreAckTest, HandlesEmptyBatchWithoutWireTraffic) {
  auto transport = OpenFake();
  auto store_result = sitos::ParamStore::Open(transport);
  ASSERT_TRUE(store_result.IsOk());
  auto store = std::move(store_result).Value();
  ASSERT_TRUE(store.PutBatch("base", {}, sitos::ParamStore::WriteOptions{}).IsOk());
  ASSERT_TRUE(
      store.PutBatch("base", {}, sitos::ParamStore::WriteOptions{.ack = false, .ack_timeout = 0ms})
          .IsOk());
  EXPECT_EQ(
      store.PutBatch("base", {}, sitos::ParamStore::WriteOptions{.ack = true, .ack_timeout = 0ms})
          .StatusCode(),
      sitos::Status::InvalidArgument);
  EXPECT_TRUE(transport->puts.empty());
  EXPECT_TRUE(transport->get_keys.empty());
}

}  // namespace
