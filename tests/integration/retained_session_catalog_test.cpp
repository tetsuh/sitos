// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Issue #108 / ADR-0036: retained-session catalog, restart reconciliation,
// deletion, and the catalog-unavailable readiness latch.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "fence_internal.hpp"
#include "fence_test_access.hpp"
#include "retained_session_support.hpp"
#include "session_catalog.hpp"
#include "sitos/ack.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "storage_node_test_access.hpp"

namespace {

using retained_session_test::CatalogTransport;
using retained_session_test::MetaState;
using retained_session_test::Query;
using retained_session_test::ScopedRoot;
using sitos::SessionLifecycleState;
using sitos::Status;
using sitos::StorageNodeReadinessReason;
using sitos::catalog_internal::SessionCatalogRecord;
using sitos::catalog_internal::SessionCatalogTestAccess;
using sitos::storage_node_test_access::StorageNodeTestAccess;

std::vector<std::byte> Bytes(std::initializer_list<int> values) {
  std::vector<std::byte> bytes;
  for (int value : values) bytes.push_back(static_cast<std::byte>(value));
  return bytes;
}

sitos::StorageNodeConfig CatalogConfig(const std::filesystem::path& root) {
  sitos::StorageNodeConfig config;
  config.prefix = "sitos";
  config.durable_root = root;
  return config;
}

class RetainedSessionCatalogTest : public ::testing::Test {
 protected:
  // Starts a fresh node generation over the same durable root, as a host restart does.
  void Restart() {
    node_.reset();
    node_ = std::make_unique<sitos::StorageNode>(transport_);
    ASSERT_TRUE(node_->Start(base_, CatalogConfig(root_.Path())).IsOk());
  }

  void SetUp() override { Restart(); }

  std::optional<SessionCatalogRecord> Record(const std::string& sid) {
    return StorageNodeTestAccess::CatalogRecord(*node_, sid);
  }

  std::filesystem::path GenerationDirectory(const std::string& sid) {
    const auto record = Record(sid);
    if (!record) return {};
    return root_.Path() / "sessions" / sid / record->generation_uuid;
  }

  CatalogTransport transport_;
  ScopedRoot root_{"catalog"};
  std::shared_ptr<sitos::InMemoryEngine> base_ = std::make_shared<sitos::InMemoryEngine>();
  std::unique_ptr<sitos::StorageNode> node_;
};

// ---------------------------------------------------------------------------
// First use, readiness, and configuration
// ---------------------------------------------------------------------------

TEST_F(RetainedSessionCatalogTest, FirstUseInitializesCatalogAndIsReady) {
  EXPECT_TRUE(std::filesystem::is_directory(root_.Path() / "catalog"));
  const auto readiness = node_->Readiness();
  EXPECT_TRUE(readiness.ready);
  EXPECT_EQ(readiness.reason, StorageNodeReadinessReason::kReady);
}

TEST(RetainedSessionCatalogConfigTest, RootAndFactoryTogetherAreRejected) {
  CatalogTransport transport;
  ScopedRoot root("config");
  sitos::StorageNode node{transport};
  auto config = CatalogConfig(root.Path());
  config.durable_buffer_engine_factory = [](std::string_view) {
    return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Err(sitos::Status::Error);
  };
  const auto started = node.Start(std::make_shared<sitos::InMemoryEngine>(), std::move(config));
  ASSERT_FALSE(started.IsOk());
  EXPECT_EQ(started.StatusCode(), Status::InvalidArgument);
  EXPECT_FALSE(node.IsStarted());
}

TEST(RetainedSessionCatalogConfigTest, StoppedNodeIsNotReady) {
  CatalogTransport transport;
  sitos::StorageNode node{transport};
  const auto readiness = node.Readiness();
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.reason, StorageNodeReadinessReason::kStopped);
}

TEST(RetainedSessionCatalogConfigTest, NodeWithoutDurableRootKeepsFactoryModeAndIsReady) {
  CatalogTransport transport;
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(std::make_shared<sitos::InMemoryEngine>(), {.prefix = "sitos"}).IsOk());
  EXPECT_TRUE(node.Readiness().ready);
  ASSERT_TRUE(node.CreateSession("session").IsOk());
  const auto retained = node.RetainSession("session");
  ASSERT_FALSE(retained.IsOk());
  EXPECT_EQ(retained.StatusCode(), Status::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Retain and restart
// ---------------------------------------------------------------------------

TEST_F(RetainedSessionCatalogTest, RetainRestartRediscoversStore) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "image/0", Bytes({1, 2, 3}));
  const auto retained = node_->RetainSession("run");
  ASSERT_TRUE(retained.IsOk()) << retained.Message();
  EXPECT_EQ(retained.Value(), SessionLifecycleState::kRetained);

  Restart();
  EXPECT_TRUE(node_->Readiness().ready);
  const auto get = Query(transport_, "sitos/buffers/run/durable/image/0");
  ASSERT_TRUE(get.result.IsOk()) << get.result.Message();
  ASSERT_EQ(get.replies.size(), 1u);
  EXPECT_EQ(get.replies[0].second, Bytes({1, 2, 3}));
  const auto list = Query(transport_, "sitos/buffers/run/durable/**");
  ASSERT_TRUE(list.result.IsOk());
  ASSERT_EQ(list.replies.size(), 1u);
  EXPECT_EQ(list.replies[0].first, "sitos/buffers/run/durable/image/0");
  EXPECT_EQ(MetaState(transport_, "run"), "retained");
}

TEST_F(RetainedSessionCatalogTest, RetainTwiceReturnsRetainedWithoutRewriting) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  const auto first = Record("run");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->state, SessionLifecycleState::kRetained);

  const auto again = node_->RetainSession("run");
  ASSERT_TRUE(again.IsOk()) << again.Message();
  EXPECT_EQ(again.Value(), SessionLifecycleState::kRetained);
  EXPECT_EQ(Record("run"), first);
}

TEST_F(RetainedSessionCatalogTest, RetainedSurvivesRepeatedRestarts) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  const auto generation = Record("run")->generation_uuid;
  for (int restart = 0; restart < 2; ++restart) {
    Restart();
    const auto record = Record("run");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, SessionLifecycleState::kRetained);
    EXPECT_EQ(record->generation_uuid, generation);
  }
}

TEST_F(RetainedSessionCatalogTest, PreviousInstanceActiveBecomesOrphaned) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "partial", Bytes({9}));
  ASSERT_EQ(Record("run")->state, SessionLifecycleState::kActive);

  Restart();
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kOrphaned);
  EXPECT_FALSE(record->orphaned_at.empty());
  EXPECT_EQ(MetaState(transport_, "run"), "orphaned");
  const auto get = Query(transport_, "sitos/buffers/run/durable/partial");
  ASSERT_TRUE(get.result.IsOk());
  ASSERT_EQ(get.replies.size(), 1u);
  EXPECT_EQ(get.replies[0].second, Bytes({9}));
}

TEST_F(RetainedSessionCatalogTest, ParameterReadsAfterRetainAndRestartAreStateLost) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutParameter("session/run", "gain", 7);
  ASSERT_EQ(Query(transport_, "sitos/session/run/gain").replies.size(), 1u);

  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  for (int phase = 0; phase < 2; ++phase) {
    for (const std::string& selector :
         {std::string("sitos/session/run/gain"), std::string("sitos/snap/run/gain"),
          std::string("sitos/session/run/**")}) {
      const auto read = Query(transport_, selector);
      ASSERT_FALSE(read.result.IsOk()) << selector << " phase " << phase;
      EXPECT_EQ(read.result.StatusCode(), Status::StateLost) << selector;
      EXPECT_TRUE(read.replies.empty());
    }
    // A later write to the released overlay is not applied and does not resurrect it.
    transport_.PutParameter("session/run", "gain", 8);
    Restart();
  }
  // An unknown Session keeps its zero-reply NotFound meaning.
  EXPECT_TRUE(Query(transport_, "sitos/session/unknown/gain").result.IsOk());
  EXPECT_TRUE(Query(transport_, "sitos/session/unknown/gain").replies.empty());
}

TEST_F(RetainedSessionCatalogTest, RetainDoesNotStopTheRunningDurableReceiver) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  transport_.PutBuffer("run", "late", Bytes({5}));
  EXPECT_EQ(Query(transport_, "sitos/buffers/run/durable/late").replies.size(), 1u);
}

TEST_F(RetainedSessionCatalogTest, ReconciledStoresServeReadsOnly) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  Restart();
  transport_.PutBuffer("run", "after-restart", Bytes({4}));
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/after-restart").replies.empty());
}

TEST_F(RetainedSessionCatalogTest, ReconciledDualCapabilitySessionRestoresNoEphemeralOrFence) {
  ASSERT_TRUE(
      node_->CreateSession("run", {.durable_buffers = true, .ephemeral_buffers = true}).IsOk());
  transport_.PutEphemeral("run", "live", Bytes({1}));
  ASSERT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node_), 1u);
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  const auto generation = sitos::fence_internal::ParseFenceUuid(Record("run")->generation_uuid);
  ASSERT_TRUE(generation.has_value());

  Restart();
  transport_.PutEphemeral("run", "live", Bytes({2}));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::BufferApplicationCount(*node_), 0u);

  const auto token = sitos::GenerateAckToken();
  transport_.Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "run", *generation, sitos::BufferClass::Durable,
      sitos::fence_internal::GenerateFenceUuid(), sitos::AckDurability::Synced, 1, token));
  const auto result = sitos::fence_test_access::FenceTestAccess::FindAckResult(*node_, token);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, Status::InvalidArgument);
  EXPECT_EQ(Query(transport_, "sitos/buffers/run/durable/**").result.StatusCode(), Status::Ok);
}

TEST_F(RetainedSessionCatalogTest, RetainPreconditions) {
  const auto unknown = node_->RetainSession("unknown");
  ASSERT_FALSE(unknown.IsOk());
  EXPECT_EQ(unknown.StatusCode(), Status::NotFound);

  ASSERT_TRUE(node_->CreateSession("ephemeral-only", {.ephemeral_buffers = true}).IsOk());
  const auto ephemeral = node_->RetainSession("ephemeral-only");
  ASSERT_FALSE(ephemeral.IsOk());
  EXPECT_EQ(ephemeral.StatusCode(), Status::InvalidArgument);

  ASSERT_TRUE(node_->CreateSession("orphan", {.durable_buffers = true}).IsOk());
  Restart();
  const auto orphaned = node_->RetainSession("orphan");
  ASSERT_FALSE(orphaned.IsOk());
  EXPECT_EQ(orphaned.StatusCode(), Status::InvalidArgument);

  node_->Stop();
  const auto stopped = node_->RetainSession("orphan");
  ASSERT_FALSE(stopped.IsOk());
  EXPECT_EQ(stopped.StatusCode(), Status::InvalidArgument);
}

TEST_F(RetainedSessionCatalogTest, EphemeralOnlySessionHasNoRecord) {
  ASSERT_TRUE(node_->CreateSession("live", {.ephemeral_buffers = true}).IsOk());
  EXPECT_FALSE(Record("live").has_value());
  Restart();
  EXPECT_FALSE(Record("live").has_value());
  EXPECT_EQ(MetaState(transport_, "live"), std::nullopt);
}

// ---------------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------------

TEST_F(RetainedSessionCatalogTest, CloseRemovesGenerationDirectoryAndRecordsDeleted) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "k", Bytes({1}));
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  const auto directory = GenerationDirectory("run");
  ASSERT_TRUE(std::filesystem::is_directory(directory));

  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  EXPECT_FALSE(std::filesystem::exists(directory));
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kDeleted);
  EXPECT_FALSE(record->deleted_at.empty());
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/k").replies.empty());
  EXPECT_EQ(MetaState(transport_, "run"), std::nullopt);

  Restart();
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

TEST_F(RetainedSessionCatalogTest, CloseOfReconciledOrphanPurgesItsStore) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "k", Bytes({1}));
  const auto directory = GenerationDirectory("run");
  Restart();
  ASSERT_EQ(Record("run")->state, SessionLifecycleState::kOrphaned);
  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  EXPECT_FALSE(std::filesystem::exists(directory));
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

TEST_F(RetainedSessionCatalogTest, RecreateRules) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  const auto first_generation = Record("run")->generation_uuid;
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  EXPECT_FALSE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  Restart();
  EXPECT_FALSE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());

  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  const auto second = Record("run");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->state, SessionLifecycleState::kActive);
  EXPECT_NE(second->generation_uuid, first_generation);
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/**").replies.empty());
}

TEST_F(RetainedSessionCatalogTest, RetainDuringCloseDoesNotOverwriteDeleting) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  std::optional<sitos::Result<SessionLifecycleState>> retained;
  ASSERT_TRUE(StorageNodeTestAccess::SetCatalogCheckpoint(*node_, [&](std::string_view point) {
    if (point == "close:before_begin_close") {
      retained = node_->RetainSession("run");
      EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleting);
    }
  }));
  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(retained->StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

TEST_F(RetainedSessionCatalogTest, CloseDuringCreateLeavesTheActiveRecord) {
  std::optional<sitos::Result<void>> closed;
  ASSERT_TRUE(StorageNodeTestAccess::SetCatalogCheckpoint(*node_, [&](std::string_view point) {
    if (point == "create:active_written") closed = node_->CloseSession("run");
  }));
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(closed.has_value());
  EXPECT_EQ(closed->Error(), std::make_error_code(std::errc::operation_in_progress));
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kActive);
  EXPECT_EQ(MetaState(transport_, "run"), "active");
}

TEST_F(RetainedSessionCatalogTest, StaleDeletionCompletionKeepsTheNewGeneration) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  const auto first_generation = Record("run")->generation_uuid;
  bool nested = false;
  std::optional<sitos::Result<void>> second_close;
  std::optional<sitos::Result<void>> recreated;
  ASSERT_TRUE(StorageNodeTestAccess::SetCatalogCheckpoint(*node_, [&](std::string_view point) {
    if (point != "close:before_remove" || nested) return;
    nested = true;
    // The first closer has erased the live record and pauses before removal: a second
    // closer finishes the deletion, and the sid is created again.
    second_close = node_->CloseSession("run");
    recreated = node_->CreateSession("run", {.durable_buffers = true});
  }));
  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  ASSERT_TRUE(second_close.has_value() && second_close->IsOk());
  ASSERT_TRUE(recreated.has_value() && recreated->IsOk());
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kActive);
  EXPECT_NE(record->generation_uuid, first_generation);
  EXPECT_TRUE(std::filesystem::is_directory(GenerationDirectory("run")));
}

TEST_F(RetainedSessionCatalogTest, DurableWritesStopAfterTheCatalogLatch) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  transport_.PutBuffer("run", "before", Bytes({1}));
  ASSERT_TRUE(StorageNodeTestAccess::LatchCatalogUnavailable(*node_));
  transport_.PutBuffer("run", "after", Bytes({2}));
  Restart();
  ASSERT_TRUE(node_->Readiness().ready);
  EXPECT_EQ(Query(transport_, "sitos/buffers/run/durable/before").replies.size(), 1u);
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/after").replies.empty());
}

TEST_F(RetainedSessionCatalogTest, RemovalFailureRecordsDeleteFailedThenRetrySucceeds) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "k", Bytes({1}));
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  const auto directory = GenerationDirectory("run");

  // Block removal: an open file on Windows, a read-only parent on POSIX. The guard
  // restores write permission even when an assertion stops the test early.
  std::optional<std::ofstream> held;
  struct WritableAgain {
    std::filesystem::path path;
    ~WritableAgain() {
      std::error_code ignored;
      std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, ignored);
    }
  } restore{directory.parent_path()};
#if defined(_WIN32)
  held.emplace(directory / "held.bin");
  ASSERT_TRUE(held->is_open());
#else
  if (::geteuid() == 0) GTEST_SKIP() << "directory permission bits do not bind root";
  std::filesystem::permissions(directory.parent_path(), std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::remove);
#endif
  const auto failed = node_->CloseSession("run");
  ASSERT_FALSE(failed.IsOk());
  auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kDeleteFailed);
  ASSERT_TRUE(record->failure.has_value());
  EXPECT_EQ(record->failure->operation, "remove_directory");
  EXPECT_EQ(MetaState(transport_, "run"), "delete_failed");
  EXPECT_FALSE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());

  // delete_failed is not retried automatically at Start.
  Restart();
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleteFailed);

#if defined(_WIN32)
  held.reset();
#else
  std::filesystem::permissions(directory.parent_path(), std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
#endif
  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  EXPECT_FALSE(std::filesystem::exists(directory));
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

TEST_F(RetainedSessionCatalogTest, FailedStoreOpenRollsBackThroughDeletion) {
  std::filesystem::create_directories(root_.Path() / "sessions");
  { std::ofstream blocker(root_.Path() / "sessions" / "run"); }
  const auto created = node_->CreateSession("run", {.durable_buffers = true});
  ASSERT_FALSE(created.IsOk());
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kDeleted);
  EXPECT_TRUE(std::filesystem::is_regular_file(root_.Path() / "sessions" / "run"));
  EXPECT_TRUE(node_->Readiness().ready);
}

TEST_F(RetainedSessionCatalogTest, ReadsRacingDeletionAreQuiescedOrUnknown) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  transport_.PutBuffer("run", "k", Bytes({1}));
  std::vector<std::size_t> replies_during_removal;
  ASSERT_TRUE(StorageNodeTestAccess::SetCatalogCheckpoint(*node_, [&](std::string_view point) {
    if (point == "close:before_remove") {
      replies_during_removal.push_back(
          Query(transport_, "sitos/buffers/run/durable/k").replies.size());
      transport_.PutBuffer("run", "racing", Bytes({2}));
    }
  }));
  ASSERT_TRUE(node_->CloseSession("run").IsOk());
  ASSERT_EQ(replies_during_removal.size(), 1u);
  EXPECT_EQ(replies_during_removal[0], 0u);
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

TEST_F(RetainedSessionCatalogTest, UnknownDirectoriesSurviveStart) {
  node_->Stop();
  const auto ghost = root_.Path() / "sessions" / "ghost" / "0b5d7f3e-2c4a-4e6b-8f1d-3a5c7e9b1d2f";
  std::filesystem::create_directories(ghost);
  { std::ofstream(ghost / "data.bin") << "payload"; }
  { std::ofstream(root_.Path() / "sessions" / "stray.txt") << "stray"; }

  Restart();
  EXPECT_TRUE(node_->Readiness().ready);
  EXPECT_TRUE(std::filesystem::exists(ghost / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(root_.Path() / "sessions" / "stray.txt"));
  EXPECT_TRUE(Query(transport_, "sitos/buffers/ghost/durable/**").replies.empty());
}

TEST_F(RetainedSessionCatalogTest, MetaSessionReportsLifecycleState) {
  ASSERT_TRUE(node_->CreateSession("run", {.durable_buffers = true}).IsOk());
  EXPECT_EQ(MetaState(transport_, "run"), "active");
  ASSERT_TRUE(node_->RetainSession("run").IsOk());
  EXPECT_EQ(MetaState(transport_, "run"), "retained");
}

// ---------------------------------------------------------------------------
// Catalog unavailability
// ---------------------------------------------------------------------------

class DegradedCatalogTest : public ::testing::Test {
 protected:
  void StartDegradedByCorruptRecord() {
    {
      sitos::StorageNode first{transport_};
      ASSERT_TRUE(first.Start(base_, CatalogConfig(root_.Path())).IsOk());
      ASSERT_TRUE(first.CreateSession("kept", {.durable_buffers = true}).IsOk());
      ASSERT_TRUE(first.RetainSession("kept").IsOk());
    }
    ASSERT_TRUE(
        SessionCatalogTestAccess::PutRawOffline(root_.Path(), "session/broken", "not a record")
            .IsOk());
    ASSERT_TRUE(node_.Start(base_, CatalogConfig(root_.Path())).IsOk());
  }

  CatalogTransport transport_;
  ScopedRoot root_{"degraded"};
  std::shared_ptr<sitos::InMemoryEngine> base_ = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node_{transport_};
};

TEST_F(DegradedCatalogTest, CorruptCatalogStartsLiveButNotReady) {
  StartDegradedByCorruptRecord();
  EXPECT_TRUE(node_.IsStarted());
  const auto readiness = node_.Readiness();
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.reason, StorageNodeReadinessReason::kCatalogUnavailable);
}

TEST_F(DegradedCatalogTest, DegradedRejectsEveryCatalogOperation) {
  StartDegradedByCorruptRecord();
  for (const auto& options :
       {sitos::SessionOptions{.durable_buffers = true},
        sitos::SessionOptions{.ephemeral_buffers = true}, sitos::SessionOptions{}}) {
    const auto created = node_.CreateSession("new", options);
    ASSERT_FALSE(created.IsOk());
    EXPECT_EQ(created.StatusCode(), Status::CatalogUnavailable);
  }
  EXPECT_EQ(node_.RetainSession("kept").StatusCode(), Status::CatalogUnavailable);
  EXPECT_EQ(node_.CloseSession("kept").StatusCode(), Status::CatalogUnavailable);
  for (const std::string& selector : {std::string("sitos/buffers/kept/durable/k"),
                                      std::string("sitos/buffers/kept/durable/**")}) {
    const auto read = Query(transport_, selector);
    ASSERT_FALSE(read.result.IsOk()) << selector;
    EXPECT_EQ(read.result.StatusCode(), Status::CatalogUnavailable) << selector;
  }
}

TEST_F(DegradedCatalogTest, DegradedLatchIsMonotonic) {
  StartDegradedByCorruptRecord();
  for (int attempt = 0; attempt < 3; ++attempt) {
    EXPECT_EQ(node_.CreateSession("new", {.durable_buffers = true}).StatusCode(),
              Status::CatalogUnavailable);
    EXPECT_EQ(Query(transport_, "sitos/buffers/kept/durable/**").result.StatusCode(),
              Status::CatalogUnavailable);
    EXPECT_FALSE(node_.Readiness().ready);
  }
}

TEST_F(DegradedCatalogTest, DegradedKeepsBaseRoutes) {
  StartDegradedByCorruptRecord();
  transport_.PutParameter("base", "gain", 3);
  const auto read = Query(transport_, "sitos/base/gain");
  ASSERT_TRUE(read.result.IsOk());
  EXPECT_EQ(read.replies.size(), 1u);
}

TEST_F(DegradedCatalogTest, RepairThenRestartBecomesReady) {
  StartDegradedByCorruptRecord();
  node_.Stop();
  ASSERT_TRUE(SessionCatalogTestAccess::DeleteRawOffline(root_.Path(), "session/broken").IsOk());
  ASSERT_TRUE(node_.Start(base_, CatalogConfig(root_.Path())).IsOk());
  EXPECT_TRUE(node_.Readiness().ready);
  EXPECT_EQ(StorageNodeTestAccess::CatalogRecord(node_, "kept")->state,
            SessionLifecycleState::kRetained);
}

TEST_F(DegradedCatalogTest, MissingCatalogBesideSessionsIsNotReady) {
  std::filesystem::create_directories(root_.Path() / "sessions" / "left");
  ASSERT_TRUE(node_.Start(base_, CatalogConfig(root_.Path())).IsOk());
  EXPECT_FALSE(node_.Readiness().ready);
  EXPECT_FALSE(std::filesystem::exists(root_.Path() / "catalog"));
  EXPECT_TRUE(std::filesystem::exists(root_.Path() / "sessions" / "left"));
}

TEST_F(DegradedCatalogTest, HeldCatalogLockIsNotReady) {
  sitos::StorageNode owner{transport_};
  ASSERT_TRUE(owner.Start(base_, CatalogConfig(root_.Path())).IsOk());
  CatalogTransport second_transport;
  sitos::StorageNode second{second_transport};
  ASSERT_TRUE(second.Start(base_, CatalogConfig(root_.Path())).IsOk());
  EXPECT_FALSE(second.Readiness().ready);
  EXPECT_TRUE(owner.Readiness().ready);
}

}  // namespace
