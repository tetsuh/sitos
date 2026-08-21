// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode acknowledgement behavior under ADR-0028: token claim before
// mutation, typed AckResultV1 retention, meta/ack/<uuid> replies, duplicate and
// collision handling, batch confirmed prefix, ring eviction, Stop clearing, and
// concurrent recording/querying.

#include "sitos/storage_node.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
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
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/logging.hpp"
#include "sitos/param_value.hpp"
#include "storage_node_test_access.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace sitos {
namespace {

using storage_node_test_access::StorageNodeTestAccess;

struct CapturedLogRecord {
  LogLevel level;
  std::string message;
};

class CaptureSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back({record.level, std::string(record.message)});
  }
  std::vector<CapturedLogRecord> Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
  }
  bool Contains(std::string_view message) const {
    for (const auto& r : Records()) {
      if (r.message == message) return true;
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<CapturedLogRecord> records_;
};

// Records every Put and can fail, throw, or block at a chosen call index.
class ScriptedEngine final : public StorageEngine {
 public:
  bool Put(std::string_view key, Bytes value) override {
    std::size_t index = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      index = puts_.size();
      puts_.emplace_back(key);
    }
    if (throw_at && index == *throw_at) throw std::runtime_error("engine exploded");
    if (fail_at && index == *fail_at) return false;
    if (block_at && index == *block_at) {
      std::unique_lock<std::mutex> lock(mutex_);
      blocked_ = true;
      cv_.notify_all();
      cv_.wait(lock, [this] { return released_; });
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return backing_.Put(key, value);
  }
  bool Delete(std::string_view key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++deletes_;
    return backing_.Delete(key);
  }
  bool Get(std::string_view key, const EntrySink& sink) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return backing_.Get(key, sink);
  }
  bool List(std::string_view prefix, const EntrySink& sink) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return backing_.List(prefix, sink);
  }

  std::vector<std::string> Puts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return puts_;
  }
  std::size_t Deletes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deletes_;
  }
  bool WaitUntilBlocked() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(3), [this] { return blocked_; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  std::optional<std::size_t> fail_at;
  std::optional<std::size_t> throw_at;
  std::optional<std::size_t> block_at;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  InMemoryEngine backing_;
  std::vector<std::string> puts_;
  std::size_t deletes_ = 0;
  bool blocked_ = false;
  bool released_ = false;
};

class FakeTransport final : public Transport {
 public:
  struct ReplyRecord {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
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
    subscriber_callback = std::move(callback);
    return Result<Subscription>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }
  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)> callback) override {
    query_callback = std::move(callback);
    return Result<Queryable>::Ok(
        transport_test_access::DeclarationHandleTestAccess::MakeQueryable([] {}));
  }

  void Deliver(std::string key, TransportSample::Kind kind, std::span<const std::byte> payload,
               std::string encoding, AckAttachmentObservation ack) {
    TransportSample sample{std::move(key), payload, Encoding{std::move(encoding)}, std::move(ack),
                           kind};
    subscriber_callback(sample);
  }

  std::vector<ReplyRecord> Query(std::string keyexpr) {
    std::vector<ReplyRecord> replies;
    auto query = TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, Encoding encoding) {
          replies.push_back({std::string(key),
                             std::vector<std::byte>(payload.begin(), payload.end()),
                             std::move(encoding)});
          return Result<void>::Ok();
        });
    query.keyexpr = std::move(keyexpr);
    query_callback(query);
    return replies;
  }

  std::function<void(const TransportSample&)> subscriber_callback;
  std::function<void(TransportQuery&)> query_callback;
};

std::vector<std::byte> MakeBatch(std::initializer_list<std::pair<std::string, ParamValue>> entries) {
  return EncodeBatch(
      std::span<const std::pair<std::string, ParamValue>>(entries.begin(), entries.size()));
}

const std::string kV1 = std::string(Encoding::kSitosV1);
const std::string kBatch = std::string(Encoding::kSitosV1Batch);

struct Harness {
  std::shared_ptr<ScriptedEngine> engine = std::make_shared<ScriptedEngine>();
  std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
  FakeTransport transport;
  StorageNode node;

  void Start() {
    ASSERT_TRUE(node.Start(engine, transport, {.prefix = "sitos", .log_sink = sink}).IsOk());
  }

  std::vector<std::byte> value_bytes = ParamValue(std::int64_t{42}).Encode();

  AckToken Put(std::string key, std::optional<AckToken> token = std::nullopt,
               std::string encoding = kV1) {
    const AckToken t = token.value_or(GenerateAckToken());
    transport.Deliver(std::move(key), TransportSample::Kind::Put, value_bytes, std::move(encoding),
                      t);
    return t;
  }

  std::string AckKey(const AckToken& token) { return "sitos/meta/ack/" + FormatAckToken(token); }

  std::optional<AckResultV1> QueryAck(const AckToken& token) {
    const auto replies = transport.Query(AckKey(token));
    if (replies.empty()) return std::nullopt;
    EXPECT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0].key, AckKey(token)) << "reply uses the exact query key";
    EXPECT_EQ(replies[0].encoding.id, Encoding::kSitosV1Ack);
    auto decoded = DecodeAckResult(replies[0].payload);
    EXPECT_TRUE(decoded.IsOk()) << decoded.Message();
    if (!decoded.IsOk()) return std::nullopt;
    return decoded.Value();
  }

  std::size_t RegistryEntries() {
    return StorageNodeTestAccess::AckRegistryEntryCount(node).value_or(9999);
  }
};

AckResultV1 ExpectResult(std::optional<AckResultV1> result, AckOperationKind kind, Status status,
                         std::uint32_t applied, std::uint32_t failed_index) {
  EXPECT_TRUE(result.has_value());
  if (!result) return {};
  EXPECT_EQ(result->operation_kind, kind);
  EXPECT_EQ(result->status, status);
  EXPECT_EQ(result->durability, AckDurability::Applied);
  EXPECT_EQ(result->applied_count, applied);
  EXPECT_EQ(result->failed_index, failed_index);
  EXPECT_EQ(result->through_sequence, 0u);
  EXPECT_EQ(result->failed_sequence, kAckNoFailedSequence);
  return *result;
}

// ---------------------------------------------------------------------------

TEST(StorageNodeAckTest, AckRoundTripRecordsTypedResult) {
  Harness h;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");

  EXPECT_EQ(h.engine->Puts(), (std::vector<std::string>{"a"}));
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
  EXPECT_EQ(h.RegistryEntries(), 1u);
  EXPECT_TRUE(h.sink->Records().empty());
}

TEST(StorageNodeAckTest, AbsentAttachmentDoesNotPopulateRegistry) {
  Harness h;
  h.Start();
  h.transport.Deliver("sitos/base/a", TransportSample::Kind::Put, h.value_bytes, kV1,
                      AckAttachmentAbsent{});
  EXPECT_EQ(h.engine->Puts(), (std::vector<std::string>{"a"}));
  EXPECT_EQ(h.RegistryEntries(), 0u);
}

TEST(StorageNodeAckTest, MalformedAttachmentIsRejectedBeforeApplication) {
  Harness h;
  h.Start();
  h.transport.Deliver("sitos/base/a", TransportSample::Kind::Put, h.value_bytes, kV1,
                      AckAttachmentMalformed{});
  EXPECT_TRUE(h.engine->Puts().empty());
  EXPECT_EQ(h.RegistryEntries(), 0u);
  EXPECT_TRUE(h.sink->Contains("malformed ack attachment; sample rejected"));
}

TEST(StorageNodeAckTest, NonCanonicalOrUnknownTokenQueriesYieldZeroReplies) {
  Harness h;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  ASSERT_TRUE(h.QueryAck(token).has_value());

  std::string upper = h.AckKey(token);
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_TRUE(h.transport.Query(upper).empty()) << "only canonical lowercase names a result";
  EXPECT_TRUE(h.transport.Query(h.AckKey(GenerateAckToken())).empty()) << "unknown token";
  EXPECT_TRUE(h.transport.Query("sitos/meta/ack/not-a-uuid").empty());
}

TEST(StorageNodeAckTest, ProcessingTokenYieldsZeroReplies) {
  Harness h;
  h.engine->block_at = 0;
  h.Start();
  const AckToken token = GenerateAckToken();
  std::thread writer([&] { h.Put("sitos/base/a", token); });
  ASSERT_TRUE(h.engine->WaitUntilBlocked());

  EXPECT_FALSE(h.QueryAck(token).has_value()) << "Processing observes no result";
  EXPECT_EQ(h.RegistryEntries(), 1u);
  h.engine->Release();
  writer.join();
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
}

TEST(StorageNodeAckTest, DuplicateTokenNeverReapplies) {
  Harness h;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  h.Put("sitos/base/a", token);
  h.Put("sitos/base/a", token);

  EXPECT_EQ(h.engine->Puts().size(), 1u);
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
  EXPECT_EQ(h.RegistryEntries(), 1u);
  EXPECT_TRUE(h.sink->Records().empty()) << "duplicates are silent";
}

TEST(StorageNodeAckTest, FingerprintCollisionIsRejectedWithoutApplication) {
  Harness h;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  h.Put("sitos/base/b", token);  // same token, different key
  h.transport.Deliver("sitos/base/a", TransportSample::Kind::Put, ParamValue(std::int64_t{7}).Encode(),
                      kV1, token);  // same token and key, different payload

  EXPECT_EQ(h.engine->Puts(), (std::vector<std::string>{"a"}));
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
  EXPECT_TRUE(h.sink->Contains("ack token collision; sample rejected"));
  EXPECT_EQ(h.RegistryEntries(), 1u);
}

TEST(StorageNodeAckTest, EngineRejectionIsOutcomeUnknown) {
  Harness h;
  h.engine->fail_at = 0;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::OutcomeUnknown, 0, 0);
  EXPECT_TRUE(h.sink->Contains("subscriber PUT failed"));
}

TEST(StorageNodeAckTest, EngineExceptionAfterInvocationIsOutcomeUnknown) {
  Harness h;
  h.engine->throw_at = 0;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::OutcomeUnknown, 0, 0);
  EXPECT_TRUE(h.sink->Contains("subscriber callback exception"));
  // The lane is released by the completion guard: a later write is still acknowledged.
  h.engine->throw_at.reset();
  const AckToken next = h.Put("sitos/base/b");
  ExpectResult(h.QueryAck(next), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
}

TEST(StorageNodeAckTest, ValidationRejectionsCreateDefiniteResultsWithoutMutation) {
  Harness h;
  h.Start();

  const AckToken unknown_session = h.Put("sitos/session/nope/a");
  ExpectResult(h.QueryAck(unknown_session), AckOperationKind::Put, Status::NotFound, 0, 0);

  const AckToken snapshot = h.Put("sitos/snap/s1/a");
  ExpectResult(h.QueryAck(snapshot), AckOperationKind::Put, Status::ReadOnly, 0, 0);

  const AckToken meta = h.Put("sitos/meta/session/s1");
  ExpectResult(h.QueryAck(meta), AckOperationKind::Put, Status::InvalidKey, 0, 0);

  const AckToken unparsable = h.Put("sitos/nowhere/a");
  ExpectResult(h.QueryAck(unparsable), AckOperationKind::Put, Status::InvalidKey, 0, 0);

  const AckToken batch_key_wrong_encoding = h.Put("sitos/base/:batch");
  ExpectResult(h.QueryAck(batch_key_wrong_encoding), AckOperationKind::Batch,
               Status::InvalidArgument, 0, kAckNoFailedIndex);

  const AckToken deleted = GenerateAckToken();
  h.transport.Deliver("sitos/base/a", TransportSample::Kind::Delete, {}, "", deleted);
  ExpectResult(h.QueryAck(deleted), AckOperationKind::Put, Status::InvalidArgument, 0, 0);
  EXPECT_EQ(h.engine->Deletes(), 0u);

  ASSERT_TRUE(h.node.CreateSession("s1", SessionOptions{.ephemeral_buffers = true}).IsOk());
  const AckToken buffer = GenerateAckToken();
  h.transport.Deliver("sitos/buffers/s1/ephemeral/k", TransportSample::Kind::Put, h.value_bytes,
                      "zenoh/bytes", buffer);
  ExpectResult(h.QueryAck(buffer), AckOperationKind::Put, Status::InvalidArgument, 0, 0);

  EXPECT_TRUE(h.engine->Puts().empty());
}

TEST(StorageNodeAckTest, SessionWriteIsAcknowledged) {
  Harness h;
  h.Start();
  ASSERT_TRUE(h.node.CreateSession("s1").IsOk());
  const AckToken token = h.Put("sitos/session/s1/a");
  ExpectResult(h.QueryAck(token), AckOperationKind::Put, Status::Ok, 1, kAckNoFailedIndex);
  EXPECT_TRUE(h.engine->Puts().empty()) << "session writes go to the overlay";
  ASSERT_EQ(h.transport.Query("sitos/session/s1/a").size(), 1u);
}

TEST(StorageNodeAckTest, BatchSuccessReportsEntryCount) {
  Harness h;
  h.Start();
  const auto batch = MakeBatch({{"a", ParamValue(std::int64_t{1})},
                                {"b", ParamValue(std::int64_t{2})},
                                {"c", ParamValue(std::int64_t{3})}});
  const AckToken token = GenerateAckToken();
  h.transport.Deliver("sitos/base/:batch", TransportSample::Kind::Put, batch, kBatch, token);
  EXPECT_EQ(h.engine->Puts(), (std::vector<std::string>{"a", "b", "c"}));
  ExpectResult(h.QueryAck(token), AckOperationKind::Batch, Status::Ok, 3, kAckNoFailedIndex);
}

TEST(StorageNodeAckTest, BatchStopsAtFirstEngineFailureWithConfirmedPrefix) {
  Harness h;
  h.engine->fail_at = 1;
  h.Start();
  const auto batch = MakeBatch({{"a", ParamValue(std::int64_t{1})},
                                {"b", ParamValue(std::int64_t{2})},
                                {"c", ParamValue(std::int64_t{3})}});
  const AckToken token = GenerateAckToken();
  h.transport.Deliver("sitos/base/:batch", TransportSample::Kind::Put, batch, kBatch, token);
  EXPECT_EQ(h.engine->Puts(), (std::vector<std::string>{"a", "b"})) << "c is never attempted";
  ExpectResult(h.QueryAck(token), AckOperationKind::Batch, Status::OutcomeUnknown, 1, 1);
}

TEST(StorageNodeAckTest, BatchEntryValidationFailureNamesEntry) {
  Harness h;
  h.Start();
  const auto batch = MakeBatch({{"a", ParamValue(std::int64_t{1})},
                                {"bad key", ParamValue(std::int64_t{2})},
                                {"c", ParamValue(std::int64_t{3})}});
  const AckToken token = GenerateAckToken();
  h.transport.Deliver("sitos/base/:batch", TransportSample::Kind::Put, batch, kBatch, token);
  EXPECT_TRUE(h.engine->Puts().empty()) << "complete prevalidation precedes any mutation";
  ExpectResult(h.QueryAck(token), AckOperationKind::Batch, Status::InvalidArgument, 0, 1);
}

TEST(StorageNodeAckTest, BatchEnvelopeFailureHasNoFailedIndex) {
  Harness h;
  h.Start();
  const std::vector<std::byte> garbage = {std::byte{0xFF}, std::byte{0xFF}};
  const AckToken token = GenerateAckToken();
  h.transport.Deliver("sitos/base/:batch", TransportSample::Kind::Put, garbage, kBatch, token);
  EXPECT_TRUE(h.engine->Puts().empty());
  ExpectResult(h.QueryAck(token), AckOperationKind::Batch, Status::InvalidArgument, 0,
               kAckNoFailedIndex);
}

TEST(StorageNodeAckTest, CompletionRingEvictsOldestAfterCapacity) {
  Harness h;
  h.Start();
  constexpr std::size_t kCapacity = 4096;
  std::vector<AckToken> tokens;
  tokens.reserve(kCapacity + 1);
  for (std::size_t i = 0; i <= kCapacity; ++i) tokens.push_back(h.Put("sitos/base/k"));

  EXPECT_FALSE(h.QueryAck(tokens.front()).has_value()) << "oldest result aged out";
  EXPECT_TRUE(h.QueryAck(tokens[1]).has_value());
  EXPECT_TRUE(h.QueryAck(tokens.back()).has_value());
  EXPECT_EQ(h.RegistryEntries(), kCapacity);
  // After eviction the node no longer suppresses the evicted token: it is admitted and applied.
  h.Put("sitos/base/k", tokens.front());
  EXPECT_EQ(h.engine->Puts().size(), kCapacity + 2);
}

TEST(StorageNodeAckTest, StopClearsTokenState) {
  Harness h;
  h.Start();
  const AckToken token = h.Put("sitos/base/a");
  ASSERT_TRUE(h.QueryAck(token).has_value());
  h.node.Stop();
  EXPECT_FALSE(StorageNodeTestAccess::AckRegistryEntryCount(h.node).has_value());

  h.Start();
  EXPECT_EQ(h.RegistryEntries(), 0u);
  EXPECT_FALSE(h.QueryAck(token).has_value()) << "a later Start does not recover old results";
}

TEST(StorageNodeAckTest, ConcurrentRecordingAndQueryingIsSafe) {
  Harness h;
  h.Start();
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 200;
  std::vector<std::vector<AckToken>> tokens(kWriters);
  std::atomic<bool> stop{false};
  std::atomic<int> observed{0};

  std::thread reader([&] {
    while (!stop.load()) {
      if (h.transport.Query(h.AckKey(GenerateAckToken())).empty()) observed.fetch_add(1);
    }
  });
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const AckToken token = GenerateAckToken();
        tokens[w].push_back(token);
        h.Put("sitos/base/w" + std::to_string(w), token);
        EXPECT_TRUE(h.QueryAck(token).has_value());
      }
    });
  }
  for (auto& t : writers) t.join();
  stop.store(true);
  reader.join();

  EXPECT_EQ(h.engine->Puts().size(), static_cast<std::size_t>(kWriters * kPerWriter));
  for (const auto& list : tokens) {
    for (const auto& token : list) EXPECT_TRUE(h.QueryAck(token).has_value());
  }
  EXPECT_GT(observed.load(), 0);
}

}  // namespace
}  // namespace sitos
