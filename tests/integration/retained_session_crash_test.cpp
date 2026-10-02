// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Issue #108 / ADR-0036: a process crash after each synchronized catalog write
// is reconciled at the next Start.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "retained_session_support.hpp"
#include "session_catalog.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "storage_node_test_access.hpp"

namespace {

using retained_session_test::CatalogTransport;
using retained_session_test::Query;
using retained_session_test::ScopedRoot;
using sitos::SessionLifecycleState;
using sitos::storage_node_test_access::StorageNodeTestAccess;

int RunScenario(const std::filesystem::path& root, const std::string& scenario) {
  const std::string helper = SITOS_RETAINED_SESSION_CRASH_HELPER;
  const std::string root_text = root.string();
#if defined(_WIN32)
  return static_cast<int>(_spawnl(_P_WAIT, helper.c_str(), helper.c_str(), root_text.c_str(),
                                  scenario.c_str(), static_cast<const char*>(nullptr)));
#else
  const pid_t child = ::fork();
  if (child == 0) {
    ::execl(helper.c_str(), helper.c_str(), root_text.c_str(), scenario.c_str(),
            static_cast<char*>(nullptr));
    std::_Exit(127);
  }
  if (child < 0) return -1;
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

class RetainedSessionCrashTest : public ::testing::Test {
 protected:
  void RestartAfter(const std::string& scenario) {
    ASSERT_EQ(RunScenario(root_.Path(), scenario), 0) << scenario;
    sitos::StorageNodeConfig config;
    config.prefix = "sitos";
    config.durable_root = root_.Path();
    ASSERT_TRUE(node_.Start(std::make_shared<sitos::InMemoryEngine>(), config).IsOk());
    ASSERT_TRUE(node_.Readiness().ready);
  }

  std::optional<sitos::catalog_internal::SessionCatalogRecord> Record(const std::string& sid) {
    return StorageNodeTestAccess::CatalogRecord(node_, sid);
  }

  ScopedRoot root_{"crash"};
  CatalogTransport transport_;
  sitos::StorageNode node_{transport_};
};

TEST_F(RetainedSessionCrashTest, CrashAfterRecordBeforeStoreBecomesEmptyOrphan) {
  RestartAfter("create-record");
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kOrphaned);
  const auto list = Query(transport_, "sitos/buffers/run/durable/**");
  EXPECT_TRUE(list.result.IsOk());
  EXPECT_TRUE(list.replies.empty());
  EXPECT_TRUE(node_.CloseSession("run").IsOk());
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
}

// The generation of an active record left by the create-record scenario.
std::string ActiveGeneration(const std::filesystem::path& root) {
  auto catalog = sitos::catalog_internal::SessionCatalog::Open(
      root, "9a0c1d2e-3f40-4b5c-8d6e-7f8091a2b3c4", "2026-10-02T00:00:00Z");
  if (!catalog.IsOk()) return {};
  const auto record = catalog.Value()->Find("run");
  return record && record->state == SessionLifecycleState::kActive ? record->generation_uuid
                                                                   : std::string();
}

// A crash during orphan recovery leaves the record active, with the store either fully
// initialized or only partly created; the next Start must still recover it.
TEST_F(RetainedSessionCrashTest, RecoveryInterruptedAfterStoreInitializationResumes) {
  ASSERT_EQ(RunScenario(root_.Path(), "create-record"), 0);
  const auto generation = ActiveGeneration(root_.Path());
  ASSERT_FALSE(generation.empty());
  const auto directory = root_.Path() / "sessions" / "run" / generation;
  std::filesystem::create_directories(directory);
  ASSERT_TRUE(sitos::RocksDBEngine::Open(directory.string()).IsOk());
  ASSERT_TRUE(std::filesystem::exists(directory / "CURRENT"));

  sitos::StorageNodeConfig config;
  config.prefix = "sitos";
  config.durable_root = root_.Path();
  ASSERT_TRUE(node_.Start(std::make_shared<sitos::InMemoryEngine>(), config).IsOk());
  ASSERT_TRUE(node_.Readiness().ready);
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kOrphaned);
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/**").result.IsOk());
}

TEST_F(RetainedSessionCrashTest, RecoveryInterruptedDuringStoreInitializationResumes) {
  ASSERT_EQ(RunScenario(root_.Path(), "create-record"), 0);
  const auto generation = ActiveGeneration(root_.Path());
  ASSERT_FALSE(generation.empty());
  const auto directory = root_.Path() / "sessions" / "run" / generation;
  std::filesystem::create_directories(directory);
  { std::ofstream(directory / "LOG") << "partial initialization"; }
  ASSERT_FALSE(std::filesystem::exists(directory / "CURRENT"));

  sitos::StorageNodeConfig config;
  config.prefix = "sitos";
  config.durable_root = root_.Path();
  ASSERT_TRUE(node_.Start(std::make_shared<sitos::InMemoryEngine>(), config).IsOk());
  ASSERT_TRUE(node_.Readiness().ready);
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kOrphaned);
  EXPECT_TRUE(std::filesystem::exists(directory / "CURRENT"));
  const auto list = Query(transport_, "sitos/buffers/run/durable/**");
  EXPECT_TRUE(list.result.IsOk());
  EXPECT_TRUE(list.replies.empty());
}

TEST_F(RetainedSessionCrashTest, ActiveBecomesOrphanedAfterAbruptExit) {
  RestartAfter("active");
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kOrphaned);
  EXPECT_TRUE(Query(transport_, "sitos/buffers/run/durable/**").result.IsOk());
}

TEST_F(RetainedSessionCrashTest, RetainedWriteSurvivesAbruptExitWithSyncedBuffers) {
  RestartAfter("retain");
  const auto record = Record("run");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, SessionLifecycleState::kRetained);
  const auto get = Query(transport_, "sitos/buffers/run/durable/k");
  ASSERT_TRUE(get.result.IsOk());
  ASSERT_EQ(get.replies.size(), 1u);
  EXPECT_EQ(get.replies[0].second, (std::vector<std::byte>{std::byte{7}}));
}

TEST_F(RetainedSessionCrashTest, DeletingResumesOnStartAndTouchesOnlyItsGeneration) {
  RestartAfter("close-deleting");
  const auto run = Record("run");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, SessionLifecycleState::kDeleted);
  EXPECT_FALSE(std::filesystem::exists(root_.Path() / "sessions" / "run" / run->generation_uuid));
  const auto other = Record("other");
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->state, SessionLifecycleState::kRetained);
  const auto get = Query(transport_, "sitos/buffers/other/durable/k");
  ASSERT_EQ(get.replies.size(), 1u);
  EXPECT_EQ(get.replies[0].second, (std::vector<std::byte>{std::byte{8}}));

  ASSERT_TRUE(node_.CreateSession("run", {.durable_buffers = true}).IsOk());
  EXPECT_NE(Record("run")->generation_uuid, run->generation_uuid);
}

TEST_F(RetainedSessionCrashTest, CrashAfterRemovalBeforeDeletedWriteIsIdempotent) {
  RestartAfter("close-removed");
  EXPECT_EQ(Record("run")->state, SessionLifecycleState::kDeleted);
  EXPECT_EQ(Record("other")->state, SessionLifecycleState::kRetained);
}

}  // namespace
