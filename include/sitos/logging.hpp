// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Backend-neutral diagnostics boundary for sitos.

#ifndef SITOS_LOGGING_HPP
#define SITOS_LOGGING_HPP

#include <memory>
#include <string_view>

namespace sitos {

/// Severity assigned to a diagnostic record.
enum class LogLevel { kDebug, kInfo, kWarning, kError };

/// A non-owning diagnostic record.
///
/// The component and message views are valid only during the synchronous
/// LogSink::Write call. An asynchronous sink must copy both strings before
/// Write returns. Write may be called concurrently; sink implementations
/// must synchronize access to their mutable state.
struct LogRecord {
  LogLevel level;
  std::string_view component;
  std::string_view message;
};

/// Application-provided destination for diagnostic records.
///
/// This method intentionally is not noexcept: adapters may use third-party
/// backends that throw. EmitLog is the non-throwing boundary for sitos code.
class LogSink {
 public:
  virtual ~LogSink() = default;

  /// Consumes a record synchronously, or copies it for asynchronous delivery.
  virtual void Write(const LogRecord& record) = 0;
};

/// Returns the immutable built-in stderr sink used by omitted configurations.
/// Applications may provide their own sink without configuring global state.
std::shared_ptr<LogSink> DefaultLogSink();

/// Delivers a diagnostic if sink is non-null and contains every sink exception.
/// Logging failure never changes component results, storage state, or callback
/// control flow. A null sink is an explicit no-op.
void EmitLog(const std::shared_ptr<LogSink>& sink, LogLevel level,
             std::string_view component, std::string_view message) noexcept;

}  // namespace sitos

#endif  // SITOS_LOGGING_HPP
