// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Source-private test vocabulary for Issue #158. Production must provide the
// real component seams declared by fence_test_access.hpp; this file intentionally
// does not define a parallel Fence implementation.

#ifndef SITOS_TESTS_UNIT_FENCE_TEST_SUPPORT_HPP
#define SITOS_TESTS_UNIT_FENCE_TEST_SUPPORT_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ack_registry.hpp"
#include "fence_test_access.hpp"
#include "sitos/ack.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/session.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace sitos::fence_test {

inline constexpr std::string_view kPrefix = "sitos";
inline constexpr std::string_view kSid = "s1";
inline constexpr std::chrono::milliseconds kDeadline{100};

inline constexpr FenceUuid kPublisherA{{
    std::byte{0x8b},
    std::byte{0x8f},
    std::byte{0x3a},
    std::byte{0x62},
    std::byte{0x7d},
    std::byte{0xd5},
    std::byte{0x4c},
    std::byte{0x40},
    std::byte{0x8a},
    std::byte{0x2b},
    std::byte{0x28},
    std::byte{0xf7},
    std::byte{0x13},
    std::byte{0x31},
    std::byte{0xfe},
    std::byte{0x41},
}};
inline constexpr FenceUuid kPublisherB{{
    std::byte{0x70},
    std::byte{0xd9},
    std::byte{0x8f},
    std::byte{0x35},
    std::byte{0x92},
    std::byte{0x17},
    std::byte{0x42},
    std::byte{0x5b},
    std::byte{0x8c},
    std::byte{0x9e},
    std::byte{0x14},
    std::byte{0xf6},
    std::byte{0x64},
    std::byte{0x54},
    std::byte{0x1b},
    std::byte{0x9a},
}};
inline constexpr FenceUuid kAttachGeneration{{
    std::byte{0x12},
    std::byte{0x3e},
    std::byte{0x45},
    std::byte{0x67},
    std::byte{0xe8},
    std::byte{0x9b},
    std::byte{0x42},
    std::byte{0xd3},
    std::byte{0xa4},
    std::byte{0x56},
    std::byte{0x42},
    std::byte{0x66},
    std::byte{0x14},
    std::byte{0x17},
    std::byte{0x40},
    std::byte{0x00},
}};
inline constexpr FenceUuid kSessionGeneration{{
    std::byte{0x6f},
    std::byte{0x1c},
    std::byte{0x2d},
    std::byte{0x3e},
    std::byte{0x4a},
    std::byte{0x5b},
    std::byte{0x4c},
    std::byte{0x6d},
    std::byte{0x8e},
    std::byte{0x9f},
    std::byte{0x01},
    std::byte{0x23},
    std::byte{0x45},
    std::byte{0x67},
    std::byte{0x89},
    std::byte{0xab},
}};

inline AckToken Token(std::byte tail) {
  AckToken token{{std::byte{0x55}, std::byte{0x0e}, std::byte{0x84}, std::byte{0x00},
                  std::byte{0xe2}, std::byte{0x9b}, std::byte{0x41}, std::byte{0xd4},
                  std::byte{0xa7}, std::byte{0x16}, std::byte{0x44}, std::byte{0x66},
                  std::byte{0x55}, std::byte{0x44}, std::byte{0x40}, tail}};
  return token;
}

inline std::filesystem::path Fixture(std::string_view name) {
  return std::filesystem::path(SITOS_FENCE_FIXTURE_DIR) / name;
}

inline std::vector<std::byte> ReadHexFixture(std::string_view name) {
  std::ifstream stream(Fixture(name));
  std::vector<std::byte> bytes;
  std::string token;
  while (stream >> token) {
    bytes.push_back(static_cast<std::byte>(std::stoul(token, nullptr, 16)));
  }
  return bytes;
}

/// Deterministic test-only Transport. It records real Transport calls and lets
/// Fence test access inject callback completion only after a chosen marker has
/// been submitted. It contains no Fence receiver/publisher/result policy.
class DeterministicFenceTransport final : public Transport {
 public:
  bool SupportsFenceProfile() const noexcept override {
    std::scoped_lock lock(mutex_);
    return fence_profile_supported_;
  }
  std::uint64_t FenceGeneration() const noexcept override { return generation(); }
  std::shared_ptr<fence_internal::FenceDispatchCoordinator> FenceDispatcher() noexcept override {
    return fence_dispatcher_;
  }

  struct PutRecord {
    std::string key;
    std::vector<std::byte> payload;
    Encoding encoding;
    PutOptions options;
  };

  Result<void> Put(std::string_view key, std::span<const std::byte> payload, Encoding encoding,
                   PutOptions options) override {
    PutRecord record{std::string(key), std::vector<std::byte>(payload.begin(), payload.end()),
                     std::move(encoding), std::move(options)};
    std::function<void(const PutRecord&)> observer;
    {
      std::scoped_lock lock(mutex_);
      ++put_count_;
      if (record.encoding.id == Encoding::kSitosV1Fence) {
        ++marker_count_;
        if (record.options.ack_token.has_value())
          pending_tokens_.push_back(*record.options.ack_token);
        if (gate_marker_completion_) {
          pending_marker_ = record;
          return put_result_;
        }
      } else {
        data_sequences_.push_back(
            fence_test_access::FenceTestAccess::FenceSequence(record.options));
      }
      observer = put_observer_;
    }
    if (observer) observer(record);
    return put_result_;
  }

  Result<void> Delete(std::string_view, PutOptions) override { return Result<void>::Ok(); }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds) override {
    std::function<void(TransportQuery&)> queryable;
    {
      std::scoped_lock lock(mutex_);
      ++ack_query_count_;
      queryable = queryable_;
    }
    if (!queryable) return Result<void>::Ok();
    auto query = TransportQuery::ForTesting(
        [this, &sink](std::string_view key, std::span<const std::byte> payload, Encoding encoding) {
          static_cast<void>(sink(key, payload, std::move(encoding)));
          std::scoped_lock lock(mutex_);
          ++storage_node_result_count_;
          return Result<void>::Ok();
        });
    query.keyexpr = std::string(keyexpr);
    queryable(query);
    {
      std::unique_lock lock(mutex_);
      if (gate_ack_query_return_) {
        gate_ack_query_return_ = false;
        ack_query_observed_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return release_ack_query_return_; });
      }
    }
    return Result<void>::Ok();
  }

  Result<Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const TransportSample&)> callback) override {
    std::function<void(const TransportSample&)> previous;
    {
      std::scoped_lock lock(mutex_);
      previous = std::exchange(subscriber_, std::move(callback));
    }
    return Result<Subscription>::Ok(Subscription{});
  }

  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)> callback) override {
    std::function<void(TransportQuery&)> previous;
    {
      std::scoped_lock lock(mutex_);
      previous = std::exchange(queryable_, std::move(callback));
    }
    return Result<Queryable>::Ok(Queryable{});
  }

  void Deliver(const TransportSample& sample) {
    std::function<void(const TransportSample&)> callback;
    {
      std::scoped_lock lock(mutex_);
      callback = subscriber_;
    }
    if (callback) callback(sample);
  }

  void DeliverAsync(TransportSample sample) {
    std::thread([this, sample = std::move(sample)] { Deliver(sample); }).detach();
  }

  void SetPutObserver(std::function<void(const PutRecord&)> observer) {
    std::scoped_lock lock(mutex_);
    put_observer_ = std::move(observer);
  }
  void SetDataSubmissionResult(Result<void> result) {
    std::scoped_lock lock(mutex_);
    put_result_ = std::move(result);
  }
  void GateAckQueryReturn() {
    std::scoped_lock lock(mutex_);
    gate_ack_query_return_ = true;
    ack_query_observed_ = false;
    release_ack_query_return_ = false;
  }
  void WaitForAckQueryObservation() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return ack_query_observed_; });
  }
  void ReleaseAckQueryReturn() {
    std::scoped_lock lock(mutex_);
    release_ack_query_return_ = true;
    condition_.notify_all();
  }
  void GateMarkerCompletion() {
    std::scoped_lock lock(mutex_);
    gate_marker_completion_ = true;
  }
  void ReleaseMarkerCompletion() {
    std::optional<PutRecord> marker;
    std::function<void(const PutRecord&)> observer;
    {
      std::scoped_lock lock(mutex_);
      marker = std::move(pending_marker_);
      gate_marker_completion_ = false;
      observer = put_observer_;
    }
    if (marker.has_value() && observer) observer(*marker);
  }
  void ReplaceGeneration() {
    std::scoped_lock lock(mutex_);
    ++generation_;
  }
  void SetFenceProfileSupported(bool supported) {
    std::scoped_lock lock(mutex_);
    fence_profile_supported_ = supported;
  }
  void MarkWaiterPublished() {
    std::scoped_lock lock(mutex_);
    marker_waiter_was_published_ = true;
  }

  [[nodiscard]] std::size_t DataSubmissionCount() const {
    std::scoped_lock lock(mutex_);
    return data_sequences_.size();
  }
  [[nodiscard]] std::size_t MarkerSubmissionCount() const {
    std::scoped_lock lock(mutex_);
    return marker_count_;
  }
  [[nodiscard]] std::size_t MarkerCount() const { return MarkerSubmissionCount(); }
  [[nodiscard]] std::vector<std::uint64_t> DataSequences() const {
    std::scoped_lock lock(mutex_);
    return data_sequences_;
  }
  [[nodiscard]] std::size_t TokenCount() const {
    std::scoped_lock lock(mutex_);
    return pending_tokens_.size();
  }
  [[nodiscard]] bool MarkerWaiterWasPublished() const {
    std::scoped_lock lock(mutex_);
    return marker_waiter_was_published_;
  }
  [[nodiscard]] std::size_t AckQueryCount() const {
    std::scoped_lock lock(mutex_);
    return ack_query_count_;
  }
  [[nodiscard]] std::size_t StorageNodeResultCount() const {
    std::scoped_lock lock(mutex_);
    return storage_node_result_count_;
  }
  [[nodiscard]] std::uint64_t generation() const {
    std::scoped_lock lock(mutex_);
    return generation_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::shared_ptr<fence_internal::FenceDispatchCoordinator> fence_dispatcher_ =
      std::make_shared<fence_internal::FenceDispatchCoordinator>();
  Result<void> put_result_ = Result<void>::Ok();
  std::function<void(const TransportSample&)> subscriber_;
  std::function<void(TransportQuery&)> queryable_;
  std::function<void(const PutRecord&)> put_observer_;
  std::optional<PutRecord> pending_marker_;
  std::vector<std::uint64_t> data_sequences_;
  std::vector<AckToken> pending_tokens_;
  std::size_t put_count_ = 0;
  std::size_t marker_count_ = 0;
  std::size_t ack_query_count_ = 0;
  std::size_t storage_node_result_count_ = 0;
  bool gate_marker_completion_ = false;
  bool gate_ack_query_return_ = false;
  bool ack_query_observed_ = false;
  bool release_ack_query_return_ = false;
  bool fence_profile_supported_ = true;
  bool marker_waiter_was_published_ = false;
  std::uint64_t generation_ = 1;
};

inline std::shared_ptr<DeterministicFenceTransport> MakeTransport() {
  return std::make_shared<DeterministicFenceTransport>();
}

inline Result<ParamCache> OpenAttachedCache(
    const std::shared_ptr<DeterministicFenceTransport>& transport) {
  ClientConfig config;
  config.prefix = std::string(kPrefix);
  auto cache = ParamCache::Open(transport, std::move(config));
  if (!cache.IsOk()) return cache;
  auto attach = cache.Value().Attach(kSid);
  if (!attach.IsOk()) return Result<ParamCache>::ErrFrom(attach);
  return cache;
}

inline SessionOptions DurableSessionOptions() {
  return SessionOptions{.durable_buffers = true, .ephemeral_buffers = true};
}

inline std::unique_ptr<StorageNode> StartNode(
    const std::shared_ptr<DeterministicFenceTransport>& transport,
    DurableBufferEngineFactory durable_factory = {}) {
  if (!durable_factory) {
    durable_factory = [](std::string_view) {
      return Result<std::unique_ptr<StorageEngine>>::Ok(std::make_unique<InMemoryEngine>());
    };
  }
  auto node = std::make_unique<StorageNode>(*transport);
  const auto started =
      node->Start(std::make_shared<InMemoryEngine>(),
                  StorageNodeConfig{.prefix = std::string(kPrefix),
                                    .durable_buffer_engine_factory = std::move(durable_factory)});
  if (!started.IsOk()) return nullptr;
  return node;
}

inline AckResultV1 FenceResult(Status status, AckDurability durability, std::uint64_t through,
                               std::uint64_t failed_sequence = kAckNoFailedSequence) {
  return {AckOperationKind::Fence, status,  durability,      0,
          kAckNoFailedIndex,       through, failed_sequence, ""};
}

}  // namespace sitos::fence_test

#endif  // SITOS_TESTS_UNIT_FENCE_TEST_SUPPORT_HPP
