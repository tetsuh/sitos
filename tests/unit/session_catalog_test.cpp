// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Durable session catalog records and storage (ADR-0036 §D1-§D4).

#include "src/session_catalog.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "tests/integration/retained_session_support.hpp"

namespace {

using sitos::SessionLifecycleState;
using sitos::Status;
using sitos::catalog_internal::DecodeSessionRecord;
using sitos::catalog_internal::EncodeSessionRecord;
using sitos::catalog_internal::SessionCatalog;
using sitos::catalog_internal::SessionCatalogRecord;

constexpr std::string_view kGeneration = "3f2b8c1e-0d4a-4b6f-9c2e-7a1d5e8f0b93";
constexpr std::string_view kInstance = "9a0c1d2e-3f40-4b5c-8d6e-7f8091a2b3c4";

SessionCatalogRecord ActiveRecord(std::string sid = "session") {
  SessionCatalogRecord record;
  record.sid = std::move(sid);
  record.generation_uuid = std::string(kGeneration);
  record.state = SessionLifecycleState::kActive;
  record.owner_instance_id = std::string(kInstance);
  record.durable = true;
  record.ephemeral = false;
  record.created_at = "2026-10-01T00:00:00.000Z";
  return record;
}

class ScopedRoot {
 public:
  explicit ScopedRoot(std::string_view label) {
    static std::atomic<unsigned int> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
                             ("sitos-catalog-" + std::string(label) + "-" + std::to_string(stamp) +
                              "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error != std::errc::file_exists) throw std::system_error(error, "create root");
    }
    throw std::runtime_error("unable to allocate a temporary catalog root");
  }
  ~ScopedRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScopedRoot(const ScopedRoot&) = delete;
  ScopedRoot& operator=(const ScopedRoot&) = delete;
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Record codec (always built)
// ---------------------------------------------------------------------------

TEST(SessionCatalogRecordTest, EncodesCanonicalVersionedJson) {
  EXPECT_EQ(EncodeSessionRecord(ActiveRecord()),
            R"({"schema_version":1,"sid":"session",)"
            R"("generation_uuid":"3f2b8c1e-0d4a-4b6f-9c2e-7a1d5e8f0b93","state":"active",)"
            R"("owner_instance_id":"9a0c1d2e-3f40-4b5c-8d6e-7f8091a2b3c4","durable":true,)"
            R"("ephemeral":false,"created_at":"2026-10-01T00:00:00.000Z","retained_at":null,)"
            R"("orphaned_at":null,"deleting_at":null,"deleted_at":null,"failure_operation":null,)"
            R"("failure_category":null,"failure_code":null})");
}

// A record in each lifecycle state with exactly the metadata that state requires.
SessionCatalogRecord RecordIn(SessionLifecycleState state) {
  auto record = ActiveRecord();
  record.state = state;
  record.ephemeral = true;
  const auto failure =
      sitos::catalog_internal::SessionCatalogFailure{"remove_directory", "filesystem", 13};
  switch (state) {
    case SessionLifecycleState::kActive:
      break;
    case SessionLifecycleState::kRetained:
      record.retained_at = "2026-10-01T00:00:01.000Z";
      break;
    case SessionLifecycleState::kOrphaned:
      record.orphaned_at = "2026-10-01T00:00:02.000Z";
      break;
    case SessionLifecycleState::kDeleting:
      record.retained_at = "2026-10-01T00:00:01.000Z";
      record.deleting_at = "2026-10-01T00:00:03.000Z";
      record.failure = failure;  // a retry keeps the previous diagnostics until it succeeds
      break;
    case SessionLifecycleState::kDeleteFailed:
      record.deleting_at = "2026-10-01T00:00:03.000Z";
      record.failure = failure;
      break;
    case SessionLifecycleState::kDeleted:
      record.orphaned_at = "2026-10-01T00:00:02.000Z";
      record.deleting_at = "2026-10-01T00:00:03.000Z";
      record.deleted_at = "2026-10-01T00:00:04.000Z";
      break;
  }
  return record;
}

constexpr SessionLifecycleState kAllStates[] = {
    SessionLifecycleState::kActive,       SessionLifecycleState::kRetained,
    SessionLifecycleState::kOrphaned,     SessionLifecycleState::kDeleting,
    SessionLifecycleState::kDeleteFailed, SessionLifecycleState::kDeleted};

TEST(SessionCatalogRecordTest, RoundTripsEveryLifecycleStateAndFailure) {
  for (const auto state : kAllStates) {
    const auto record = RecordIn(state);
    const auto decoded = DecodeSessionRecord("session", EncodeSessionRecord(record));
    ASSERT_TRUE(decoded.has_value()) << static_cast<int>(state);
    EXPECT_EQ(*decoded, record);
  }
}

TEST(SessionCatalogRecordTest, RejectsMissingOrForeignLifecycleMetadata) {
  const auto rejects = [](SessionCatalogRecord record, const char* why) {
    EXPECT_FALSE(DecodeSessionRecord("session", EncodeSessionRecord(record)).has_value()) << why;
  };
  auto record = RecordIn(SessionLifecycleState::kRetained);
  record.retained_at.clear();
  rejects(record, "retained without retained_at");
  record = RecordIn(SessionLifecycleState::kOrphaned);
  record.orphaned_at.clear();
  rejects(record, "orphaned without orphaned_at");
  record = RecordIn(SessionLifecycleState::kDeleting);
  record.deleting_at.clear();
  rejects(record, "deleting without deleting_at");
  record = RecordIn(SessionLifecycleState::kDeleteFailed);
  record.failure.reset();
  rejects(record, "delete_failed without diagnostics");
  record = RecordIn(SessionLifecycleState::kDeleteFailed);
  record.deleting_at.clear();
  rejects(record, "delete_failed without deleting_at");
  record = RecordIn(SessionLifecycleState::kDeleted);
  record.deleted_at.clear();
  rejects(record, "deleted without deleted_at");
  record = RecordIn(SessionLifecycleState::kDeleted);
  record.failure =
      sitos::catalog_internal::SessionCatalogFailure{"remove_directory", "filesystem", 13};
  rejects(record, "deleted with diagnostics");
  record = RecordIn(SessionLifecycleState::kActive);
  record.retained_at = "2026-10-01T00:00:01.000Z";
  rejects(record, "active with a transition timestamp");
  record = RecordIn(SessionLifecycleState::kRetained);
  record.failure =
      sitos::catalog_internal::SessionCatalogFailure{"remove_directory", "filesystem", 13};
  rejects(record, "retained with diagnostics");
}

TEST(SessionCatalogRecordTest, RejectsNonCanonicalOrInconsistentRecords) {
  const std::string valid = EncodeSessionRecord(ActiveRecord());
  ASSERT_TRUE(DecodeSessionRecord("session", valid).has_value());
  EXPECT_FALSE(DecodeSessionRecord("other", valid).has_value()) << "key and sid differ";

  auto replace = [&valid](std::string_view from, std::string_view to) {
    std::string text = valid;
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::logic_error("fixture text not found");
    text.replace(at, from.size(), to);
    return text;
  };
  for (const std::string& rejected : {
           std::string(),
           std::string("{}"),
           replace(R"("schema_version":1)", R"("schema_version":2)"),
           replace(R"("state":"active")", R"("state":"sealed")"),
           replace(R"("sid":"session")", R"("sid":"bad sid")"),
           replace(R"("generation_uuid":"3f2b)", R"("generation_uuid":"XX2b)"),
           replace(R"("durable":true)", R"("durable":1)"),
           replace(R"("created_at":"2026-10-01T00:00:00.000Z")", R"("created_at":null)"),
           replace(R"("failure_code":null})", R"("failure_code":null,"extra":0})"),
           replace(R"({"schema_version":1,)", R"({ "schema_version":1,)"),
           valid + " ",
       }) {
    EXPECT_FALSE(DecodeSessionRecord("session", rejected).has_value()) << rejected;
  }
}

TEST(SessionCatalogRecordTest, AcceptsOnlyIso8601UtcTimestamps) {
  for (const std::string_view accepted :
       {"2026-10-01T00:00:00Z", "2026-10-01T23:59:60Z", "2026-10-01T00:00:00.5Z",
        "2026-10-01T00:00:00.123456789Z", "2028-02-29T00:00:00Z", "2000-02-29T00:00:00Z"}) {
    auto record = RecordIn(SessionLifecycleState::kRetained);
    record.created_at = std::string(accepted);
    record.retained_at = std::string(accepted);
    EXPECT_TRUE(DecodeSessionRecord("session", EncodeSessionRecord(record)).has_value())
        << accepted;
  }
  for (const std::string_view rejected :
       {"T", "Z", "2026-10-01", "2026-10-01T00:00:00", "2026-13-01T00:00:00Z",
        "2026-10-00T00:00:00Z", "2026-10-01T24:00:00Z", "2026-10-01T00:60:00Z",
        "2026-10-01T00:00:61Z", "2026-10-01 00:00:00Z", "2026-10-01T00:00:00.Z",
        "2026-10-01T00:00:00.1234567890Z", "2026-10-01T00:00:00+09:00", "26-10-01T00:00:00Z",
        "2026-02-29T00:00:00Z", "2100-02-29T00:00:00Z", "2026-04-31T00:00:00Z",
        "2026-02-30T00:00:00Z"}) {
    auto created = ActiveRecord();
    created.created_at = std::string(rejected);
    EXPECT_FALSE(DecodeSessionRecord("session", EncodeSessionRecord(created)).has_value())
        << "created_at " << rejected;
    auto retained = RecordIn(SessionLifecycleState::kRetained);
    retained.retained_at = std::string(rejected);
    EXPECT_FALSE(DecodeSessionRecord("session", EncodeSessionRecord(retained)).has_value())
        << "retained_at " << rejected;
  }
}

TEST(SessionCatalogRecordTest, RejectsSecondSpellingsOfNullAndZero) {
  const std::string retained = EncodeSessionRecord(RecordIn(SessionLifecycleState::kRetained));
  auto empty_timestamp = retained;
  const auto at = empty_timestamp.find(R"("orphaned_at":null)");
  ASSERT_NE(at, std::string::npos);
  empty_timestamp.replace(at, std::string_view(R"("orphaned_at":null)").size(),
                          R"("orphaned_at":"")");
  EXPECT_FALSE(DecodeSessionRecord("session", empty_timestamp).has_value());

  auto failed = RecordIn(SessionLifecycleState::kDeleteFailed);
  failed.failure->code = 0;
  auto negative_zero = EncodeSessionRecord(failed);
  const auto code = negative_zero.find(R"("failure_code":0)");
  ASSERT_NE(code, std::string::npos);
  negative_zero.replace(code, std::string_view(R"("failure_code":0)").size(),
                        R"("failure_code":-0)");
  EXPECT_TRUE(DecodeSessionRecord("session", EncodeSessionRecord(failed)).has_value());
  EXPECT_FALSE(DecodeSessionRecord("session", negative_zero).has_value());
}

TEST(SessionCatalogRecordTest, RejectsRecordsWithoutTheDurableRoute) {
  auto record = ActiveRecord();
  record.durable = false;
  EXPECT_FALSE(DecodeSessionRecord("session", EncodeSessionRecord(record)).has_value());
}

#if !defined(SITOS_WITH_ROCKSDB)

// ADR-0036 §D1: catalog mode requires RocksDB, so a RocksDB-OFF build refuses it.
TEST(SessionCatalogTest, RocksDbOffBuildRejectsDurableRoot) {
  ScopedRoot root("rocksdb-off");
  retained_session_test::CatalogTransport transport;
  sitos::StorageNode node{transport};
  sitos::StorageNodeConfig config;
  config.prefix = "sitos";
  config.durable_root = root.Path();
  const auto started = node.Start(std::make_shared<sitos::InMemoryEngine>(), std::move(config));
  ASSERT_FALSE(started.IsOk());
  EXPECT_EQ(started.StatusCode(), Status::InvalidArgument);
  EXPECT_FALSE(node.IsStarted());
  EXPECT_FALSE(node.Readiness().ready);
  EXPECT_FALSE(std::filesystem::exists(root.Path() / "catalog"));
}

#endif  // !SITOS_WITH_ROCKSDB

#if defined(SITOS_WITH_ROCKSDB)

// ---------------------------------------------------------------------------
// Catalog storage (RocksDB builds)
// ---------------------------------------------------------------------------

TEST(SessionCatalogTest, FirstUseOnEmptyRootCreatesSchemaAndInstance) {
  ScopedRoot root("first-use");
  auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  EXPECT_TRUE(opened.Value()->Records().empty());
  EXPECT_TRUE(std::filesystem::is_directory(root.Path() / "catalog"));
}

TEST(SessionCatalogTest, TransitionsSurviveReopen) {
  ScopedRoot root("reopen");
  {
    auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
    ASSERT_TRUE(opened.IsOk()) << opened.Message();
    auto record = ActiveRecord();
    ASSERT_TRUE(opened.Value()->Put(record).IsOk());
    record.state = SessionLifecycleState::kRetained;
    record.retained_at = "2026-10-01T00:00:01.000Z";
    ASSERT_TRUE(opened.Value()->Put(record).IsOk());
    ASSERT_TRUE(opened.Value()->Put(ActiveRecord("second")).IsOk());
  }
  auto reopened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:05.000Z");
  ASSERT_TRUE(reopened.IsOk()) << reopened.Message();
  const auto first = reopened.Value()->Find("session");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->state, SessionLifecycleState::kRetained);
  EXPECT_EQ(first->retained_at, "2026-10-01T00:00:01.000Z");
  EXPECT_TRUE(reopened.Value()->Find("second").has_value());
  EXPECT_EQ(reopened.Value()->Records().size(), 2u);
}

TEST(SessionCatalogTest, MissingCatalogBesideSessionsIsUnavailable) {
  ScopedRoot root("missing");
  std::filesystem::create_directories(root.Path() / "sessions" / "session");
  auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
  ASSERT_FALSE(opened.IsOk());
  EXPECT_EQ(opened.StatusCode(), Status::CatalogUnavailable);
  EXPECT_FALSE(std::filesystem::exists(root.Path() / "catalog"));
}

TEST(SessionCatalogTest, CorruptRecordIsUnavailable) {
  ScopedRoot root("corrupt");
  {
    auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
    ASSERT_TRUE(opened.IsOk());
    ASSERT_TRUE(sitos::catalog_internal::SessionCatalogTestAccess::PutRaw(
                    *opened.Value(), "session/session", "not a record")
                    .IsOk());
  }
  auto reopened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:01.000Z");
  ASSERT_FALSE(reopened.IsOk());
  EXPECT_EQ(reopened.StatusCode(), Status::CatalogUnavailable);
}

TEST(SessionCatalogTest, UnknownSchemaIsUnavailable) {
  ScopedRoot root("schema");
  {
    auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
    ASSERT_TRUE(opened.IsOk());
    ASSERT_TRUE(sitos::catalog_internal::SessionCatalogTestAccess::PutRaw(*opened.Value(), "schema",
                                                                          R"({"schema_version":2})")
                    .IsOk());
  }
  auto reopened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:01.000Z");
  ASSERT_FALSE(reopened.IsOk());
  EXPECT_EQ(reopened.StatusCode(), Status::CatalogUnavailable);
}

TEST(SessionCatalogTest, MissingSchemaInExistingCatalogIsNotRepaired) {
  ScopedRoot root("no-schema");
  {
    auto opened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
    ASSERT_TRUE(opened.IsOk());
    ASSERT_TRUE(
        sitos::catalog_internal::SessionCatalogTestAccess::DeleteRaw(*opened.Value(), "schema")
            .IsOk());
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto reopened = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:01.000Z");
    ASSERT_FALSE(reopened.IsOk()) << "attempt " << attempt;
    EXPECT_EQ(reopened.StatusCode(), Status::CatalogUnavailable);
  }
}

TEST(SessionCatalogTest, SecondOpenOfTheSameRootIsUnavailable) {
  ScopedRoot root("lock");
  auto first = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:00.000Z");
  ASSERT_TRUE(first.IsOk());
  auto second = SessionCatalog::Open(root.Path(), kInstance, "2026-10-01T00:00:01.000Z");
  ASSERT_FALSE(second.IsOk());
  EXPECT_EQ(second.StatusCode(), Status::CatalogUnavailable);
}

#endif  // SITOS_WITH_ROCKSDB

}  // namespace
