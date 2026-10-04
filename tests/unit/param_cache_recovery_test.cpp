// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ParamCache Session liveness and recovery (Issue #20, ADR-0037).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "param_cache_test_access.hpp"
#include "sitos/param_cache.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace {

using namespace std::chrono_literals;

using sitos::Encoding;
using sitos::ParamCache;
using sitos::ParamValue;
using sitos::PutOptions;
using sitos::Queryable;
using sitos::Result;
using sitos::Status;
using sitos::Subscription;
using sitos::Transport;
using sitos::TransportQuery;
using sitos::TransportSample;
using Access = sitos::param_cache_test_access::ParamCacheTestAccess;
using Kind = sitos::TransportSample::Kind;

constexpr std::string_view kLiveSelector = "sitos/meta/live/session/s1/*";
constexpr std::string_view kSnapshotQuery = "sitos/snap/s1/**";
constexpr std::string_view kOverlayQuery = "sitos/session/s1/**";
const std::string kGeneration1 = "sitos/meta/live/session/s1/11111111-1111-4111-8111-111111111111";
const std::string kGeneration2 = "sitos/meta/live/session/s1/22222222-2222-4222-8222-222222222222";

// A Transport fake with the ADR-0037 liveliness capability. Tokens listed in `history` are
// announced synchronously inside the liveliness subscriber declaration, as Zenoh does for a
// token of the same session.
class LivelinessFakeTransport final : public Transport {
 public:
  struct Reply {
    std::string key;
    std::vector<std::byte> payload;
  };

  bool SupportsLiveliness() const noexcept override { return supports_liveliness; }

  Result<void> Put(std::string_view key, std::span<const std::byte>, Encoding,
                   PutOptions) override {
    std::lock_guard lock(mutex);
    puts.emplace_back(key);
    return Result<void>::Ok();
  }
  Result<void> Delete(std::string_view, PutOptions) override { return Result<void>::Ok(); }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds) override {
    std::function<void(std::size_t)> hook;
    std::vector<Reply> replies_copy;
    Result<void> result = Result<void>::Ok();
    std::size_t ordinal = 0;
    {
      std::lock_guard lock(mutex);
      ordinal = ++get_count;
      hook = get_hook;
      replies_copy = replies[std::string(keyexpr)];
      if (failing_gets > 0) {
        --failing_gets;
        result = Result<void>::Err(Status::Timeout, "injected get failure");
      }
    }
    if (hook) hook(ordinal);
    if (!result.IsOk()) return result;
    const Encoding encoding{std::string(Encoding::kSitosV1)};
    for (const auto& reply : replies_copy) {
      if (!sink(reply.key, reply.payload, encoding)) break;
    }
    return result;
  }

  Result<Subscription> DeclareSubscriber(std::string_view keyexpr,
                                         std::function<void(const TransportSample&)>) override {
    std::lock_guard lock(mutex);
    declarations.emplace_back(keyexpr);
    return Result<Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([] {}));
  }

  Result<Queryable> DeclareQueryable(std::string_view,
                                     std::function<void(TransportQuery&)>) override {
    return Result<Queryable>::Ok(Queryable{});
  }

  Result<Subscription> DeclareLivelinessSubscriber(
      std::string_view keyexpr, std::function<void(const TransportSample&)> callback) override {
    std::vector<std::string> announced;
    {
      std::lock_guard lock(mutex);
      declarations.push_back("live:" + std::string(keyexpr));
      if (fail_liveliness_declaration) {
        return Result<Subscription>::Err(Status::Disconnected, "injected declaration failure");
      }
      liveliness_callback = callback;
      announced = history;
    }
    for (const auto& key : announced) callback(TransportSample{key, {}, {}, {}, Kind::Put});
    return Result<Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription(
            [this] { ++liveliness_resets; }));
  }

  void EmitLiveliness(const std::string& key, Kind kind) {
    std::function<void(const TransportSample&)> callback;
    {
      std::lock_guard lock(mutex);
      callback = liveliness_callback;
    }
    ASSERT_TRUE(callback);
    callback(TransportSample{key, {}, {}, {}, kind});
  }

  void SetSnapshot(std::vector<std::pair<std::string, std::int64_t>> values) {
    std::vector<Reply> encoded;
    for (const auto& [key, value] : values) {
      encoded.push_back({"sitos/snap/s1/" + key, ParamValue(value).Encode()});
    }
    std::lock_guard lock(mutex);
    replies[std::string(kSnapshotQuery)] = std::move(encoded);
  }

  void SetOverlay(std::vector<std::pair<std::string, std::int64_t>> values) {
    std::vector<Reply> encoded;
    for (const auto& [key, value] : values) {
      encoded.push_back({"sitos/session/s1/" + key, ParamValue(value).Encode()});
    }
    std::lock_guard lock(mutex);
    replies[std::string(kOverlayQuery)] = std::move(encoded);
  }

  std::size_t GetCount() {
    std::lock_guard lock(mutex);
    return get_count;
  }

  std::mutex mutex;
  bool supports_liveliness = true;
  bool fail_liveliness_declaration = false;
  std::vector<std::string> history;
  std::vector<std::string> declarations;
  std::vector<std::string> puts;
  std::unordered_map<std::string, std::vector<Reply>> replies;
  std::function<void(std::size_t)> get_hook;
  std::function<void(const TransportSample&)> liveliness_callback;
  std::size_t get_count = 0;
  std::size_t failing_gets = 0;
  std::atomic<int> liveliness_resets{0};
};

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

ParamCache OpenCache(const std::shared_ptr<LivelinessFakeTransport>& transport) {
  auto opened = ParamCache::Open(transport);
  EXPECT_TRUE(opened.IsOk()) << opened.Message();
  return std::move(opened).Value();
}

std::int64_t Read(const ParamCache& cache, std::string_view key) {
  return cache.GetOr<std::int64_t>(key, -1).Value();
}

// Blocks the calling Get until Release(); used to park the recovery worker inside a rebuild.
class GetGate {
 public:
  void Block() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
  }
  bool WaitEntered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s, [this] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

TEST(ParamCacheRecoveryTest, IsStaleIsFalseForDetachedMovedFromAndLivelinessFreeCaches) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  auto cache = OpenCache(transport);
  EXPECT_FALSE(cache.IsStale());

  transport->supports_liveliness = false;
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  EXPECT_FALSE(cache.IsStale());
  for (const auto& declaration : transport->declarations) {
    EXPECT_FALSE(declaration.starts_with("live:")) << declaration;
  }

  auto moved = std::move(cache);
  EXPECT_FALSE(cache.IsStale());  // NOLINT(bugprone-use-after-move): moved-from contract
  EXPECT_FALSE(moved.IsStale());
}

TEST(ParamCacheRecoveryTest, AttachBindsAnAnnouncedGenerationWithoutAnExtraFetch) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);

  ASSERT_TRUE(cache.Attach("s1").IsOk());

  EXPECT_FALSE(cache.IsStale());
  ASSERT_FALSE(transport->declarations.empty());
  EXPECT_EQ(transport->declarations.front(), "live:" + std::string(kLiveSelector));
  EXPECT_EQ(Read(cache, "a"), 1);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(transport->GetCount(), 2U);
}

TEST(ParamCacheRecoveryTest, AttachWithoutATokenStartsStaleAndRebuildsWhenAGenerationAppears) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->SetSnapshot({{"old", 1}});
  auto cache = OpenCache(transport);

  ASSERT_TRUE(cache.Attach("s1").IsOk());
  EXPECT_TRUE(cache.IsStale());
  EXPECT_EQ(Read(cache, "old"), 1);

  transport->SetSnapshot({{"fresh", 2}, {"overlaid", 3}});
  transport->SetOverlay({{"overlaid", 4}, {"overlay_only", 5}});
  transport->EmitLiveliness(kGeneration1, Kind::Put);

  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
  EXPECT_EQ(Read(cache, "fresh"), 2);
  EXPECT_EQ(Read(cache, "overlaid"), 4) << "the rebuild did not apply the overlay over the snapshot";
  EXPECT_EQ(Read(cache, "overlay_only"), 5);
  EXPECT_EQ(Read(cache, "old"), -1);
  EXPECT_EQ(transport->GetCount(), 4U);
}

TEST(ParamCacheRecoveryTest, AGenerationAnnouncedDuringAttachIsBoundOnlyAfterARebuild) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  {
    std::lock_guard lock(transport->mutex);
    transport->get_hook = [&transport](std::size_t ordinal) {
      // The Session is created while Attach is already reading: the first read may predate it.
      if (ordinal == 1) {
        transport->SetSnapshot({{"a", 2}});
        transport->EmitLiveliness(kGeneration1, Kind::Put);
      }
    };
  }

  ASSERT_TRUE(cache.Attach("s1").IsOk());

  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
  EXPECT_EQ(transport->GetCount(), 4U) << "Attach bound a generation announced after its first Get";
  EXPECT_EQ(Read(cache, "a"), 2);
}

TEST(ParamCacheRecoveryTest, DeleteOfTheBoundGenerationMakesTheCacheStaleAndKeepsReadsAndWrites) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->EmitLiveliness(kGeneration1, Kind::Delete);

  EXPECT_TRUE(cache.IsStale());
  EXPECT_EQ(Read(cache, "a"), 1);
  ASSERT_TRUE(cache.Put("b", std::int64_t{7}).IsOk());
  EXPECT_EQ(Read(cache, "b"), 7);
  EXPECT_EQ(transport->puts.size(), 1U);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(transport->GetCount(), 2U) << "a stale cache with no live generation must not fetch";
  EXPECT_TRUE(cache.IsStale());
}

TEST(ParamCacheRecoveryTest, TheSameGenerationReappearingRebuildsTheCache) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_TRUE(cache.Put("local", std::int64_t{9}).IsOk());

  transport->EmitLiveliness(kGeneration1, Kind::Delete);
  transport->SetSnapshot({{"a", 5}});
  transport->EmitLiveliness(kGeneration1, Kind::Put);

  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
  EXPECT_EQ(Read(cache, "a"), 5);
  EXPECT_EQ(Read(cache, "local"), -1) << "the node is authoritative after a rebuild";
}

TEST(ParamCacheRecoveryTest, APutOfAnotherGenerationRebuildsAndALateDeleteOfTheOldOneIsIgnored) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->SetSnapshot({{"a", 2}});
  transport->EmitLiveliness(kGeneration2, Kind::Put);
  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale() && Read(cache, "a") == 2; }));
  const auto gets = transport->GetCount();

  transport->EmitLiveliness(kGeneration1, Kind::Delete);

  EXPECT_FALSE(cache.IsStale());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(transport->GetCount(), gets);
  EXPECT_FALSE(cache.IsStale());
}

TEST(ParamCacheRecoveryTest, APutOfTheBoundGenerationIsIgnored) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->EmitLiveliness(kGeneration1, Kind::Put);

  EXPECT_FALSE(cache.IsStale());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(transport->GetCount(), 2U);
}

TEST(ParamCacheRecoveryTest, AnEventDuringARebuildDiscardsTheCandidate) {
  GetGate gate;  // declared first: the worker may use it until the cache is destroyed
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_TRUE(cache.IsStale());

  transport->SetSnapshot({{"a", 2}});
  {
    std::lock_guard lock(transport->mutex);
    transport->get_hook = [&gate](std::size_t ordinal) {
      if (ordinal == 3) gate.Block();  // the first Get of the first rebuild
    };
  }
  transport->EmitLiveliness(kGeneration1, Kind::Put);
  ASSERT_TRUE(gate.WaitEntered());

  // The Session is re-created while the first rebuild is in flight.
  transport->EmitLiveliness(kGeneration1, Kind::Delete);
  transport->EmitLiveliness(kGeneration2, Kind::Put);
  transport->SetSnapshot({{"a", 3}});
  gate.Release();

  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
  EXPECT_EQ(Read(cache, "a"), 3) << "the candidate read across the generation change was kept";
  EXPECT_GE(transport->GetCount(), 6U);
}

TEST(ParamCacheRecoveryTest, AFailedRebuildKeepsTheOldStateAndIsRetried) {
  GetGate gate;  // declared first: the worker may use it until the cache is destroyed
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  Access::SetRecoveryRetryInterval(cache, 10ms);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->SetSnapshot({{"a", 2}});
  {
    std::lock_guard lock(transport->mutex);
    transport->failing_gets = 2;
    transport->get_hook = [&gate](std::size_t ordinal) {
      if (ordinal == 4) gate.Block();  // the retry after the first failed rebuild
    };
  }
  transport->EmitLiveliness(kGeneration1, Kind::Put);

  ASSERT_TRUE(gate.WaitEntered()) << "the failed rebuild was not retried";
  EXPECT_TRUE(cache.IsStale());
  EXPECT_EQ(Read(cache, "a"), 1);
  gate.Release();
  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
  EXPECT_EQ(Read(cache, "a"), 2);
}

TEST(ParamCacheRecoveryTest, ReadsDoNotBlockWhileARebuildIsInFlight) {
  GetGate gate;  // declared first: the worker may use it until the cache is destroyed
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->SetSnapshot({{"a", 1}});
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  {
    std::lock_guard lock(transport->mutex);
    transport->get_hook = [&gate](std::size_t ordinal) {
      if (ordinal == 3) gate.Block();
    };
  }
  transport->EmitLiveliness(kGeneration1, Kind::Put);
  ASSERT_TRUE(gate.WaitEntered());

  EXPECT_TRUE(cache.IsStale());
  EXPECT_EQ(Read(cache, "a"), 1);
  EXPECT_TRUE(cache.Contains("a").Value());

  gate.Release();
  ASSERT_TRUE(WaitUntil([&] { return !cache.IsStale(); }));
}

TEST(ParamCacheRecoveryTest, DetachWaitsForTheInFlightGetAndStopsTheWorker) {
  GetGate gate;  // declared first: the worker may use it until the cache is destroyed
  auto transport = std::make_shared<LivelinessFakeTransport>();
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  {
    std::lock_guard lock(transport->mutex);
    transport->get_hook = [&gate](std::size_t ordinal) {
      if (ordinal == 3) gate.Block();
    };
  }
  transport->EmitLiveliness(kGeneration1, Kind::Put);
  ASSERT_TRUE(gate.WaitEntered());

  auto detached = std::async(std::launch::async, [&cache] { cache.Detach(); });
  EXPECT_EQ(detached.wait_for(100ms), std::future_status::timeout)
      << "Detach returned while the worker was still inside Get";
  gate.Release();
  ASSERT_EQ(detached.wait_for(5s), std::future_status::ready);

  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_FALSE(cache.IsStale());
  EXPECT_EQ(transport->GetCount(), 3U) << "the worker issued another Get after the stop request";
  EXPECT_EQ(transport->liveliness_resets.load(), 1);
}

TEST(ParamCacheRecoveryTest, DetachEndsTheRetryWait) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  auto cache = OpenCache(transport);
  Access::SetRecoveryRetryInterval(cache, 60s);
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  {
    std::lock_guard lock(transport->mutex);
    transport->failing_gets = 1;
  }
  transport->EmitLiveliness(kGeneration1, Kind::Put);
  ASSERT_TRUE(WaitUntil([&] { return transport->GetCount() == 3; }));
  std::this_thread::sleep_for(20ms);  // let the worker reach the retry wait

  auto detached = std::async(std::launch::async, [&cache] { cache.Detach(); });

  EXPECT_EQ(detached.wait_for(5s), std::future_status::ready)
      << "Detach waited for the retry interval";
  EXPECT_EQ(transport->GetCount(), 3U);
}

TEST(ParamCacheRecoveryTest, RecoveryContinuesAfterMoveAndStopsOnDestruction) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->SetSnapshot({{"a", 1}});
  {
    auto cache = OpenCache(transport);
    ASSERT_TRUE(cache.Attach("s1").IsOk());
    auto moved = std::move(cache);

    transport->EmitLiveliness(kGeneration1, Kind::Delete);
    EXPECT_TRUE(moved.IsStale());
    transport->SetSnapshot({{"a", 2}});
    transport->EmitLiveliness(kGeneration2, Kind::Put);
    ASSERT_TRUE(WaitUntil([&] { return !moved.IsStale(); }));
    EXPECT_EQ(Read(moved, "a"), 2);

    auto target = OpenCache(transport);
    ASSERT_TRUE(target.Attach("s1").IsOk());
    target = std::move(moved);
    EXPECT_EQ(transport->liveliness_resets.load(), 1);
    EXPECT_EQ(Read(target, "a"), 2);
  }
  EXPECT_EQ(transport->liveliness_resets.load(), 2);
}

TEST(ParamCacheRecoveryTest, AFailedLivelinessDeclarationFailsAttachAndCanBeRetried) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->fail_liveliness_declaration = true;
  auto cache = OpenCache(transport);

  const auto failed = cache.Attach("s1");

  EXPECT_EQ(failed.StatusCode(), Status::Disconnected);
  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_FALSE(cache.IsStale());
  EXPECT_EQ(transport->GetCount(), 0U);

  transport->fail_liveliness_declaration = false;
  transport->history = {kGeneration1};
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  EXPECT_FALSE(cache.IsStale());
}

TEST(ParamCacheRecoveryTest, AFailedAttachWithdrawsTheLivelinessSubscriber) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  transport->failing_gets = 1;
  auto cache = OpenCache(transport);

  EXPECT_EQ(cache.Attach("s1").StatusCode(), Status::Timeout);

  EXPECT_FALSE(Access::IsAttached(cache));
  EXPECT_FALSE(cache.IsStale());
  EXPECT_EQ(transport->liveliness_resets.load(), 1);
}

TEST(ParamCacheRecoveryTest, ForeignLivelinessKeysAreIgnored) {
  auto transport = std::make_shared<LivelinessFakeTransport>();
  transport->history = {kGeneration1};
  auto cache = OpenCache(transport);
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  transport->EmitLiveliness("sitos/meta/live/session/other/" + kGeneration2.substr(27), Kind::Put);
  transport->EmitLiveliness("sitos/meta/live/session/s1/a/b", Kind::Put);
  transport->EmitLiveliness("sitos/meta/live/session/s1/", Kind::Put);

  EXPECT_FALSE(cache.IsStale());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(transport->GetCount(), 2U);
}

}  // namespace
