// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/logging.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sitos {
namespace {

struct CapturedRecord {
  LogLevel level;
  std::string component;
  std::string message;
};

class CaptureSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::lock_guard lock(mutex_);
    record_ = CapturedRecord{record.level, std::string(record.component),
                            std::string(record.message)};
    ++write_count_;
  }

  CapturedRecord Record() const {
    std::lock_guard lock(mutex_);
    return *record_;
  }

  int WriteCount() const {
    std::lock_guard lock(mutex_);
    return write_count_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<CapturedRecord> record_;
  int write_count_ = 0;
};

class ThrowingSink final : public LogSink {
 public:
  void Write(const LogRecord&) override { throw std::runtime_error("sink failure"); }
};

TEST(LoggingTest, EmitLogDeliversRecord) {
  auto sink = std::make_shared<CaptureSink>();

  EmitLog(sink, LogLevel::kWarning, "node", "diagnostic message");

  ASSERT_EQ(sink->WriteCount(), 1);
  const CapturedRecord record = sink->Record();
  EXPECT_EQ(record.level, LogLevel::kWarning);
  EXPECT_EQ(record.component, "node");
  EXPECT_EQ(record.message, "diagnostic message");
}

TEST(LoggingTest, EmitLogIgnoresNullSink) {
  EXPECT_NO_THROW(EmitLog(nullptr, LogLevel::kInfo, "node", "ignored"));
}

TEST(LoggingTest, EmitLogContainsSinkExceptions) {
  static_assert(noexcept(EmitLog(std::shared_ptr<LogSink>{}, LogLevel::kError, "node", "error")));

  auto sink = std::make_shared<ThrowingSink>();
  EXPECT_NO_THROW(EmitLog(sink, LogLevel::kError, "node", "error"));
}

TEST(LoggingTest, RecordViewsAreCallbackScoped) {
  auto sink = std::make_shared<CaptureSink>();
  {
    std::string component = "temporary-component";
    std::string message = "temporary-message";
    EmitLog(sink, LogLevel::kDebug, component, message);
  }

  const CapturedRecord record = sink->Record();
  EXPECT_EQ(record.component, "temporary-component");
  EXPECT_EQ(record.message, "temporary-message");
}

TEST(LoggingTest, DefaultSinkIsCallable) {
  auto sink = DefaultLogSink();
  ASSERT_NE(sink, nullptr);
  EXPECT_NO_THROW(sink->Write({LogLevel::kInfo, "test", "smoke"}));
}

}  // namespace
}  // namespace sitos
