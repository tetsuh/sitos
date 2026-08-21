// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end ADR-0028 acknowledgement over a real zenoh session: the low-level
// helper submits one tokenized Put/PutBatch, StorageNode claims, applies, and
// retains the typed result, and meta/ack/<uuid> answers within the deadline.

#include "sitos/ack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ack_client.hpp"
#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_value.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

using namespace std::chrono_literals;
using sitos::AckOperationKind;
using sitos::Status;

constexpr std::string_view kPrefix = "sitos/ack_test";

class FailAtEngine final : public sitos::StorageEngine {
 public:
  bool Put(std::string_view key, sitos::Bytes value) override {
    const std::size_t index = puts_++;
    if (fail_at && index == *fail_at) return false;
    return backing_.Put(key, value);
  }
  bool Delete(std::string_view key) override { return backing_.Delete(key); }
  bool Get(std::string_view key, const sitos::EntrySink& sink) const override {
    return backing_.Get(key, sink);
  }
  bool List(std::string_view prefix, const sitos::EntrySink& sink) const override {
    return backing_.List(prefix, sink);
  }
  std::optional<std::size_t> fail_at;
  std::size_t puts() const { return puts_; }

 private:
  sitos::InMemoryEngine backing_;
  std::atomic<std::size_t> puts_{0};
};

class AckIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = sitos::MakeZenohTransport();
    ASSERT_TRUE(transport_) << "Failed to open zenoh session";
    engine_ = std::make_shared<FailAtEngine>();
  }
  void TearDown() override {
    node_.Stop();
    transport_.reset();
  }
  void StartNode() {
    ASSERT_TRUE(node_.Start(engine_, *transport_, {.prefix = std::string(kPrefix)}).IsOk());
  }
  std::string Key(std::string_view relative) {
    return std::string(kPrefix) + "/base/" + std::string(relative);
  }

  std::unique_ptr<sitos::Transport> transport_;
  std::shared_ptr<FailAtEngine> engine_;
  sitos::StorageNode node_;
};

TEST_F(AckIntegrationTest, AcknowledgedPutRoundTrip) {
  StartNode();
  const auto payload = sitos::ParamValue(std::int64_t{7}).Encode();
  const auto result = sitos::SubmitAcknowledgedWrite(
      *transport_, kPrefix, Key("a"), payload, {std::string(sitos::Encoding::kSitosV1)}, 3000ms);
  ASSERT_TRUE(result.IsOk()) << result.Message();
  EXPECT_EQ(result.Value().operation_kind, AckOperationKind::Put);
  EXPECT_EQ(result.Value().status, Status::Ok);
  EXPECT_EQ(result.Value().applied_count, 1u);

  bool stored = false;
  engine_->Get("a", [&](std::string_view, sitos::Bytes value) {
    stored = std::vector<std::byte>(value.begin(), value.end()) == payload;
    return true;
  });
  EXPECT_TRUE(stored);
}

TEST_F(AckIntegrationTest, PutAckTimesOutWhenNodeUnavailable) {
  // No StorageNode is started: the data Put is still delivered exactly once to a
  // raw subscriber, and the helper returns Timeout after the total deadline.
  std::mutex mutex;
  int observed = 0;
  auto subscription =
      transport_->DeclareSubscriber(Key("lonely"), [&](const sitos::TransportSample&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++observed;
      });
  ASSERT_TRUE(subscription.IsOk());

  const auto start = std::chrono::steady_clock::now();
  const auto result = sitos::SubmitAcknowledgedWrite(
      *transport_, kPrefix, Key("lonely"), {}, {std::string(sitos::Encoding::kSitosV1)}, 500ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Timeout);
  EXPECT_GE(elapsed, 500ms);

  std::this_thread::sleep_for(200ms);
  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(observed, 1) << "the data write is submitted exactly once and never retried";
}

TEST_F(AckIntegrationTest, BatchConfirmedPrefixIsReportedOverZenoh) {
  engine_->fail_at = 1;
  StartNode();
  const std::vector<std::pair<std::string, sitos::ParamValue>> entries = {
      {"x", sitos::ParamValue(std::int64_t{1})},
      {"y", sitos::ParamValue(std::int64_t{2})},
      {"z", sitos::ParamValue(std::int64_t{3})}};
  const auto payload = sitos::EncodeBatch(entries);
  const auto result =
      sitos::SubmitAcknowledgedWrite(*transport_, kPrefix, Key(":batch"), payload,
                                     {std::string(sitos::Encoding::kSitosV1Batch)}, 3000ms);
  ASSERT_TRUE(result.IsOk()) << result.Message();
  EXPECT_EQ(result.Value().operation_kind, AckOperationKind::Batch);
  EXPECT_EQ(result.Value().status, Status::OutcomeUnknown);
  EXPECT_EQ(result.Value().applied_count, 1u);
  EXPECT_EQ(result.Value().failed_index, 1u);
  EXPECT_EQ(engine_->puts(), 2u) << "entry z is never attempted";
}

}  // namespace
