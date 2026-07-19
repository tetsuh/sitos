// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <atomic>
#include <barrier>
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
#include "transport/declaration_handle_test_access.hpp"
#include "sitos/batch.hpp"

namespace {

using Access = sitos::param_cache_test_access::ParamCacheTestAccess;

class FakeTransport final : public sitos::Transport {
 public:
  struct Reply {
    std::string key;
    std::vector<std::byte> payload;
    sitos::Encoding encoding;
  };

  static Reply Value(std::string key, const sitos::ParamValue& value) {
    return Reply{std::move(key), value.Encode(),
                 sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}};
  }

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    std::function<void(const sitos::TransportSample&)> callback;
    bool deliver = false;
    {
      std::unique_lock lock(mutex_);
      ++put_count_;
      puts_.emplace_back(key);
      put_payloads_.emplace_back(payload.begin(), payload.end());
      put_encodings_.push_back(encoding.id);
      put_options_.push_back(options);
      submission_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return !block_submission_ || release_submission_; });
      callback = subscriber_;
      deliver = sync_delivery_;
    }
    if (deliver && callback) {
      sitos::TransportSample sample{std::string(key), payload, std::move(encoding), std::nullopt,
                                    sitos::TransportSample::Kind::Put};
      callback(sample);
    }
    std::lock_guard lock(mutex_);
    return put_result_;
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view query, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    std::vector<Reply> replies;
    sitos::Result<void> result = sitos::Result<void>::Ok();
    {
      std::lock_guard lock(mutex_);
      ++get_count_;
      replies = query.find("/snap/") != std::string_view::npos ? snapshot_replies : overlay_replies;
      result = get_result_;
    }
    for (const auto& reply : replies) {
      if (!sink(reply.key, reply.payload, reply.encoding)) break;
    }
    return result;
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    {
      std::lock_guard lock(mutex_);
      subscriber_ = std::move(callback);
    }
    return sitos::Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([this] {
          std::lock_guard lock(mutex_);
          ++reset_count_;
          condition_.notify_all();
        }));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

  void BlockSubmission() {
    std::lock_guard lock(mutex_);
    block_submission_ = true;
    release_submission_ = false;
    submission_entered_ = false;
  }

  void WaitForSubmission() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return submission_entered_; });
  }

  void ReleaseSubmission() {
    std::lock_guard lock(mutex_);
    release_submission_ = true;
    condition_.notify_all();
  }

  void WaitForReset() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return reset_count_ != 0; });
  }

  void SetSynchronousDelivery(bool enabled) {
    std::lock_guard lock(mutex_);
    sync_delivery_ = enabled;
  }

  void Deliver(const sitos::TransportSample& sample) {
    std::function<void(const sitos::TransportSample&)> callback;
    {
      std::lock_guard lock(mutex_);
      callback = subscriber_;
    }
    if (callback) callback(sample);
  }

  std::size_t PutCount() const {
    std::lock_guard lock(mutex_);
    return put_count_;
  }

  std::size_t GetCount() const {
    std::lock_guard lock(mutex_);
    return get_count_;
  }

  std::vector<std::string> Puts() const {
    std::lock_guard lock(mutex_);
    return puts_;
  }

  std::vector<std::byte> PutPayload(std::size_t index) const {
    std::lock_guard lock(mutex_);
    return put_payloads_.at(index);
  }

  std::string PutEncoding(std::size_t index) const {
    std::lock_guard lock(mutex_);
    return put_encodings_.at(index);
  }

  sitos::Result<void> PutResult() const {
    std::lock_guard lock(mutex_);
    return put_result_;
  }

  void SetPutResult(sitos::Result<void> result) {
    std::lock_guard lock(mutex_);
    put_result_ = std::move(result);
  }

  std::vector<Reply> snapshot_replies;
  std::vector<Reply> overlay_replies;
  sitos::Result<void> get_result_ = sitos::Result<void>::Ok();

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::string> puts_;
  std::vector<std::vector<std::byte>> put_payloads_;
  std::vector<std::string> put_encodings_;
  std::vector<sitos::PutOptions> put_options_;
  std::function<void(const sitos::TransportSample&)> subscriber_;
  sitos::Result<void> put_result_ = sitos::Result<void>::Ok();
  std::size_t put_count_ = 0;
  std::size_t get_count_ = 0;
  std::size_t reset_count_ = 0;
  bool sync_delivery_ = false;
  bool block_submission_ = false;
  bool release_submission_ = false;
  bool submission_entered_ = false;
};

class ParamCacheReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport = std::make_shared<FakeTransport>();
    auto result = sitos::ParamCache::Open(transport);
    ASSERT_TRUE(result.IsOk());
    cache.emplace(std::move(result).Value());
    transport->snapshot_replies.push_back(FakeTransport::Value("sitos/snap/s1/a", sitos::ParamValue(1)));
    transport->snapshot_replies.push_back(
        FakeTransport::Value("sitos/snap/s1/bytes",
                             sitos::ParamValue(std::vector<std::byte>{std::byte{1}})));
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
  EXPECT_TRUE(transport->Puts().front().find("session/s1/a") != std::string::npos);
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
  const auto before = transport->PutCount();
  EXPECT_EQ(cache->Put("bad/key/", std::int64_t{9}).StatusCode(), sitos::Status::InvalidKey);
  const std::vector<sitos::BatchEntry> invalid_entries{{"valid", sitos::ParamValue(2)},
                                                         {"bad/key/", sitos::ParamValue(3)}};
  EXPECT_EQ(cache->PutBatch(invalid_entries).StatusCode(), sitos::Status::InvalidKey);
  EXPECT_EQ(transport->PutCount(), before);

  const auto cause = std::make_error_code(std::errc::io_error);
  transport->SetPutResult(
      sitos::Result<void>::Err(sitos::Status::Disconnected, "offline", cause));
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
  transport->SetSynchronousDelivery(true);
  ASSERT_TRUE(cache->Put("sync", std::int64_t{8}).IsOk());
  EXPECT_EQ(cache->Get<std::int64_t>("sync").Value(), 8);
  EXPECT_EQ(transport->PutCount(), 1U);
}

TEST_F(ParamCacheReadTest, DelayedSelfEchoUsesSubscriberSerializationOrder) {
  ASSERT_TRUE(cache->Put("ordered", std::int64_t{1}).IsOk());
  ASSERT_TRUE(cache->Put("ordered", std::int64_t{2}).IsOk());
  ASSERT_EQ(transport->PutCount(), 2U);
  const auto payload = transport->PutPayload(0);
  sitos::TransportSample delayed{"sitos/session/s1/ordered", payload,
                                sitos::Encoding{std::string(sitos::Encoding::kSitosV1)},
                                std::nullopt, sitos::TransportSample::Kind::Put};
  transport->Deliver(delayed);
  EXPECT_EQ(cache->Get<std::int64_t>("ordered").Value(), 1);
}

TEST_F(ParamCacheReadTest, PutBatchUsesOneCanonicalSubmissionAndPreservesOrder) {
  const std::vector<sitos::BatchEntry> entries{{"b", sitos::ParamValue(2)},
                                                {"a", sitos::ParamValue(1)},
                                                {"b", sitos::ParamValue(3)}};
  ASSERT_TRUE(cache->PutBatch(entries).IsOk());
  ASSERT_EQ(transport->PutCount(), 1U);
  EXPECT_NE(transport->Puts().front().find("/session/s1/:batch"), std::string::npos);
  ASSERT_EQ(transport->PutEncoding(0), sitos::Encoding::kSitosV1Batch);
  const auto decoded = sitos::DecodeBatch(transport->PutPayload(0));
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
  EXPECT_EQ(transport->PutCount(), 0U);
}

TEST_F(ParamCacheReadTest, ContainsIsLocalAndDistinguishesAbsence) {
  EXPECT_TRUE(cache->Contains("a").Value());
  EXPECT_FALSE(cache->Contains("missing").Value());
  EXPECT_EQ(transport->PutCount(), 0U);
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
  const auto before_puts = transport->PutCount();
  const auto before_gets = transport->GetCount();
  EXPECT_TRUE(cache->GetShared("a").IsOk());
  EXPECT_TRUE(cache->Get<std::int64_t>("a").IsOk());
  EXPECT_TRUE(cache->GetOr<std::int64_t>("a", 9).IsOk());
  EXPECT_TRUE(cache->GetSpan<std::byte>("a").StatusCode() == sitos::Status::TypeMismatch);
  EXPECT_TRUE(cache->Contains("a").IsOk());
  EXPECT_TRUE(cache->List("a", [](std::string_view, const sitos::ParamValue&) { return true; }).IsOk());
  EXPECT_EQ(transport->PutCount(), before_puts);
  EXPECT_EQ(transport->GetCount(), before_gets);
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

void RunDetachLeaseTest(sitos::ParamCache& cache, FakeTransport& transport, bool batch) {
  std::mutex event_mutex;
  std::condition_variable event_condition;
  std::vector<std::string> events;
  sitos::Result<void> operation_result = sitos::Result<void>::Err(sitos::Status::Error, "unset");

  Access::SetMutationHook(cache, [&](std::size_t) {
    std::lock_guard lock(event_mutex);
    events.emplace_back("mutation");
    event_condition.notify_all();
  });
  transport.BlockSubmission();
  std::thread operation([&] {
    if (batch) {
      const std::vector<sitos::BatchEntry> entries{{"batch_lease", sitos::ParamValue(4)}};
      operation_result = cache.PutBatch(entries);
    } else {
      operation_result = cache.Put("lease", std::int64_t{4});
    }
  });
  transport.WaitForSubmission();

  const auto admitted = Access::GetGateState(cache);
  ASSERT_TRUE(admitted.has_value());
  EXPECT_TRUE(admitted->accepting);
  EXPECT_EQ(admitted->in_flight, 1U);

  std::thread detach([&] {
    cache.Detach();
    std::lock_guard lock(event_mutex);
    events.emplace_back("detach");
    event_condition.notify_all();
  });
  transport.WaitForReset();

  const auto closed = Access::GetGateState(cache);
  ASSERT_TRUE(closed.has_value());
  EXPECT_FALSE(closed->accepting);
  EXPECT_EQ(closed->in_flight, 1U);
  EXPECT_TRUE(Access::IsAttached(cache));
  const auto before_probe = transport.PutCount();
  EXPECT_EQ(cache.Put("rejected", std::int64_t{9}).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport.PutCount(), before_probe);

  transport.ReleaseSubmission();
  {
    std::unique_lock lock(event_mutex);
    ASSERT_TRUE(event_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return !events.empty() && events.front() == "mutation";
    }));
  }
  operation.join();
  detach.join();

  ASSERT_TRUE(operation_result.IsOk()) << operation_result.Message();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], "mutation");
  EXPECT_EQ(events[1], "detach");
  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_FALSE(Access::Get(cache, batch ? "batch_lease" : "lease").has_value());
}

TEST_F(ParamCacheReadTest, DetachWaitsForInFlightLocalOperation) {
  RunDetachLeaseTest(*cache, *transport, false);
}

TEST_F(ParamCacheReadTest, DetachWaitsForInFlightLocalBatchOperation) {
  RunDetachLeaseTest(*cache, *transport, true);
}

struct ReplacementHandshake {
  std::mutex mutex;
  std::condition_variable condition;
  bool mutation_applied = false;
  bool release_writer = false;
  std::ptrdiff_t readers_finished = 0;
};

TEST_F(ParamCacheReadTest, ConcurrentReadersAndWriterSwapsAreSafe) {
  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{1}}).IsOk());
  constexpr std::ptrdiff_t kReaderCount = 4;
  constexpr int kRounds = 64;
  std::atomic<bool> readers_ok = true;
  std::atomic<bool> writers_ok = true;
  ReplacementHandshake handshake;

  Access::SetMutationHook(*cache, [&](std::size_t) {
    std::unique_lock lock(handshake.mutex);
    handshake.mutation_applied = true;
    handshake.condition.notify_all();
    handshake.condition.wait(lock, [&] { return handshake.release_writer; });
  });

  const auto run_replacement_rounds = [&](const auto& replace, std::int64_t expected_base) {
    std::barrier round_start(kReaderCount + 1);
    std::barrier old_snapshots_ready(kReaderCount + 1);
    std::barrier round_complete(kReaderCount + 1);
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);
    for (std::ptrdiff_t reader = 0; reader < kReaderCount; ++reader) {
      readers.emplace_back([&] {
        for (int round = 0; round < kRounds; ++round) {
          round_start.arrive_and_wait();
          const auto held_value = cache->GetShared("a");
          const auto held_span = cache->GetSpan<std::byte>("bytes");
          if (!held_value.IsOk() || !held_span.IsOk() || held_span.Value().span.empty()) {
            readers_ok.store(false);
          }
          old_snapshots_ready.arrive_and_wait();

          {
            std::unique_lock lock(handshake.mutex);
            handshake.condition.wait(lock, [&] { return handshake.mutation_applied; });
          }
          if (held_value.IsOk() && held_value.Value()->As<std::int64_t>() < 1) {
            readers_ok.store(false);
          }
          if (held_span.IsOk() && held_span.Value().span.front() != std::byte{1}) {
            readers_ok.store(false);
          }

          const auto shared = cache->GetShared("a");
          const auto scalar = cache->Get<std::int64_t>("a");
          const auto span = cache->GetSpan<std::byte>("bytes");
          const auto contains = cache->Contains("a");
          const auto listed = cache->List("", [](std::string_view, const sitos::ParamValue&) {
            return true;
          });
          const auto expected = expected_base + round;
          if (!shared.IsOk() || shared.Value()->As<std::int64_t>() != expected || !scalar.IsOk() ||
              scalar.Value() != expected || !span.IsOk() || span.Value().span.empty() ||
              span.Value().span.front() != std::byte{1} || !contains.IsOk() || !contains.Value() ||
              !listed.IsOk()) {
            readers_ok.store(false);
          }

          {
            std::lock_guard lock(handshake.mutex);
            ++handshake.readers_finished;
            if (handshake.readers_finished == kReaderCount) {
              handshake.release_writer = true;
              handshake.condition.notify_all();
            }
          }
          round_complete.arrive_and_wait();
        }
      });
    }

    std::thread writer([&] {
      for (int round = 0; round < kRounds; ++round) {
        {
          std::lock_guard lock(handshake.mutex);
          handshake.mutation_applied = false;
          handshake.release_writer = false;
          handshake.readers_finished = 0;
        }
        round_start.arrive_and_wait();
        old_snapshots_ready.arrive_and_wait();
        replace(round);
        round_complete.arrive_and_wait();
      }
    });
    for (auto& reader : readers) reader.join();
    writer.join();
  };

  run_replacement_rounds(
      [&](int round) {
        if (!cache->Put("a", static_cast<std::int64_t>(round + 2)).IsOk()) {
          writers_ok.store(false);
        }
      },
      2);

  run_replacement_rounds(
      [&](int round) {
        const auto value = sitos::ParamValue(static_cast<std::int64_t>(round + kRounds + 2));
        transport->Deliver({"sitos/session/s1/a", value.Encode(),
                            sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, std::nullopt,
                            sitos::TransportSample::Kind::Put});
      },
      kRounds + 2);

  Access::SetMutationHook(*cache, {});
  EXPECT_TRUE(readers_ok.load());
  EXPECT_TRUE(writers_ok.load());
}

TEST_F(ParamCacheReadTest, ReadsRetainingStateSurviveDetachUnpublication) {
  std::mutex event_mutex;
  std::condition_variable event_condition;

  const auto run = [&]<typename Call, typename Verify>(Call call, Verify verify) {
    bool state_loaded = false;
    bool release_read = false;
    bool detached = false;
    using ReadResult = decltype(call());
    std::optional<ReadResult> read_result;

    Access::SetReadStateHook(*cache, [&] {
      std::unique_lock lock(event_mutex);
      state_loaded = true;
      event_condition.notify_all();
      event_condition.wait(lock, [&] { return release_read; });
    });
    std::thread reader([&] { read_result.emplace(call()); });
    {
      std::unique_lock lock(event_mutex);
      event_condition.wait(lock, [&] { return state_loaded; });
    }
    std::thread detacher([&] {
      cache->Detach();
      std::lock_guard lock(event_mutex);
      detached = true;
      event_condition.notify_all();
    });
    {
      std::unique_lock lock(event_mutex);
      event_condition.wait(lock, [&] { return detached; });
    }
    EXPECT_FALSE(Access::IsAttached(*cache));
    {
      std::lock_guard lock(event_mutex);
      release_read = true;
      event_condition.notify_all();
    }
    reader.join();
    detacher.join();
    ASSERT_TRUE(read_result.has_value());
    verify(*read_result);

    Access::SetReadStateHook(*cache, {});
    ASSERT_TRUE(cache->Attach("s1").IsOk());
    state_loaded = false;
    release_read = false;
    detached = false;
  };

  run(
      [&] { return cache->GetShared("a"); },
      [](const sitos::Result<std::shared_ptr<const sitos::ParamValue>>& result) {
        ASSERT_TRUE(result.IsOk());
        EXPECT_EQ(result.Value()->As<std::int64_t>(), 1);
      });
  run(
      [&] { return cache->Get<std::int64_t>("a"); },
      [](const sitos::Result<std::int64_t>& result) {
        ASSERT_TRUE(result.IsOk());
        EXPECT_EQ(result.Value(), 1);
      });
  run(
      [&] { return cache->GetSpan<std::byte>("bytes"); },
      [](const sitos::Result<sitos::SpanHandle<std::byte>>& result) {
        ASSERT_TRUE(result.IsOk());
        ASSERT_FALSE(result.Value().span.empty());
        EXPECT_EQ(result.Value().span.front(), std::byte{1});
      });
  run(
      [&] { return cache->Contains("a"); },
      [](const sitos::Result<bool>& result) {
        ASSERT_TRUE(result.IsOk());
        EXPECT_TRUE(result.Value());
      });

  std::vector<std::string> keys;
  run(
      [&] {
        return cache->List("", [&](std::string_view key, const sitos::ParamValue&) {
          keys.emplace_back(key);
          return true;
        });
      },
      [&](const sitos::Result<void>& result) {
        ASSERT_TRUE(result.IsOk());
        EXPECT_EQ(keys, (std::vector<std::string>{"a", "bytes"}));
      });
}

TEST_F(ParamCacheReadTest, RejectedLocalOperationsAvoidTransportAndGetOrPropagatesTypeMismatch) {
  const auto before = transport->PutCount();
  cache->Detach();
  EXPECT_EQ(cache->Put("a", std::int64_t{2}).StatusCode(), sitos::Status::InvalidArgument);
  const std::vector<sitos::BatchEntry> entries{{"a", sitos::ParamValue(2)}};
  EXPECT_EQ(cache->PutBatch(entries).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport->PutCount(), before);

  auto opened = sitos::ParamCache::Open(transport);
  ASSERT_TRUE(opened.IsOk());
  auto moved = std::move(opened).Value();
  ASSERT_TRUE(moved.Attach("s1").IsOk());
  *cache = std::move(moved);
  EXPECT_EQ(moved.Put("a", std::int64_t{2}).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(moved.PutBatch(entries).StatusCode(), sitos::Status::InvalidArgument);
  EXPECT_EQ(transport->PutCount(), before);

  ASSERT_TRUE(cache->Put("bytes", std::vector<std::byte>{std::byte{1}}).IsOk());
  EXPECT_EQ(cache->GetOr<std::int64_t>("bytes", 7).StatusCode(), sitos::Status::TypeMismatch);
  sitos::ListSink null_sink;
  EXPECT_EQ(cache->List("", null_sink).StatusCode(), sitos::Status::InvalidArgument);
}

}  // namespace
