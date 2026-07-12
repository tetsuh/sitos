// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Built-in stderr diagnostics sink.

#include "sitos/logging.hpp"

#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>

namespace sitos {
namespace {

std::string_view LevelName(LogLevel level) {
  using enum LogLevel;
  switch (level) {
    case kDebug:
      return "debug";
    case kInfo:
      return "info";
    case kWarning:
      return "warning";
    case kError:
      return "error";
  }
  return "unknown";
}

class StderrLogSink final : public LogSink {
 public:
  void Write(const LogRecord& record) override {
    std::lock_guard lock(mutex_);
    std::cerr << "[sitos][" << LevelName(record.level) << "][" << record.component << "] "
              << record.message << '\n';
  }

 private:
  std::mutex mutex_;
};

}  // namespace

std::shared_ptr<LogSink> DefaultLogSink() {
  static const std::shared_ptr<LogSink> sink = std::make_shared<StderrLogSink>();
  return sink;
}

void EmitLog(const std::shared_ptr<LogSink>& sink, LogLevel level, std::string_view component,
             std::string_view message) noexcept {
  try {
    if (sink == nullptr) return;
    sink->Write(LogRecord{level, component, message});
  } catch (...) {
    // Diagnostics must never escape into component or transport callback code.
    return;
  }
}

}  // namespace sitos
