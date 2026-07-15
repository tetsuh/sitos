// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_RESULT_HPP
#define SITOS_RESULT_HPP

#include <cassert>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "sitos/status.hpp"

namespace sitos {

/// Error state carried by Result. `cause` is always the effective, nonzero
/// native or synthesized error code returned by Result::Error().
struct ErrorInfo {
  Status status;
  std::string message;
  std::error_code cause;
};

namespace detail {

inline Status StatusFromErrorCode(const std::error_code& error) noexcept {
  // Only the portable generic category participates in the closed mapping.
  // In particular, custom categories must not be classified by their names or
  // diagnostic messages.
  if (&error.category() != &std::generic_category()) return Status::Error;
  if (error == std::errc::invalid_argument) return Status::InvalidArgument;
  if (error == std::errc::timed_out) return Status::Timeout;
  if (error == std::errc::not_connected || error == std::errc::connection_aborted ||
      error == std::errc::connection_refused || error == std::errc::connection_reset ||
      error == std::errc::network_down || error == std::errc::network_reset ||
      error == std::errc::network_unreachable || error == std::errc::host_unreachable ||
      error == std::errc::broken_pipe) {
    return Status::Disconnected;
  }
  return Status::Error;
}

inline ErrorInfo MakeErrorInfo(Status status, std::string message,
                               std::error_code cause) {
  assert(status != Status::Ok);
  if (status == Status::Ok) status = Status::Error;
  if (!cause) cause = MakeErrorCode(status);
  return ErrorInfo{status, std::move(message), cause};
}

inline ErrorInfo MakeLegacyErrorInfo(std::error_code cause) {
  assert(static_cast<bool>(cause));
  if (!cause) return MakeErrorInfo(Status::Error, {}, {});
  return MakeErrorInfo(StatusFromErrorCode(cause), {}, cause);
}

}  // namespace detail

/// A move-friendly, exclusive success-or-error result.
template <typename T>
class Result {
 public:
  static Result Ok(T value) { return Result(std::move(value)); }

  static Result Err(std::error_code cause) {
    return Result(detail::MakeLegacyErrorInfo(cause));
  }

  static Result Err(Status status, std::string message = {}, std::error_code cause = {}) {
    return Result(detail::MakeErrorInfo(status, std::move(message), cause));
  }

  bool IsOk() const noexcept { return std::holds_alternative<T>(state_); }
  explicit operator bool() const noexcept { return IsOk(); }

  /// Returns Status::Ok and an empty view for a successful result.
  Status StatusCode() const noexcept {
    if (IsOk()) return Status::Ok;
    return std::get<ErrorInfo>(state_).status;
  }

  /// Returns a view into the error state's diagnostic message.
  /// The view is invalidated by Result assignment or move.
  std::string_view Message() const noexcept {
    if (IsOk()) return {};
    return std::get<ErrorInfo>(state_).message;
  }

  /// Requires IsOk().
  const T& Value() const & {
    assert(IsOk());
    return std::get<T>(state_);
  }
  /// Requires IsOk().
  T& Value() & {
    assert(IsOk());
    return std::get<T>(state_);
  }
  /// Requires IsOk().
  T&& Value() && {
    assert(IsOk());
    return std::move(std::get<T>(state_));
  }

  /// Requires !IsOk(). The returned reference is owned by this Result.
  const std::error_code& Error() const {
    assert(!IsOk());
    return std::get<ErrorInfo>(state_).cause;
  }

  /// Propagates an error without accessing or copying source's value.
  template <typename U>
  static Result ErrFrom(const Result<U>& source) {
    assert(!source.IsOk());
    if (source.IsOk()) return Err(Status::Error);
    const auto& info = std::get<ErrorInfo>(source.state_);
    return Result(info);
  }

 private:
  explicit Result(T value) : state_(std::move(value)) {}
  explicit Result(ErrorInfo info) : state_(std::move(info)) {}

  template <typename>
  friend class Result;
  std::variant<T, ErrorInfo> state_;
};

template <>
class Result<void> {
 public:
  static Result Ok() { return Result(std::monostate{}); }

  static Result Err(std::error_code cause) {
    return Result(detail::MakeLegacyErrorInfo(cause));
  }

  static Result Err(Status status, std::string message = {}, std::error_code cause = {}) {
    return Result(detail::MakeErrorInfo(status, std::move(message), cause));
  }

  bool IsOk() const noexcept { return std::holds_alternative<std::monostate>(state_); }
  explicit operator bool() const noexcept { return IsOk(); }

  Status StatusCode() const noexcept {
    if (IsOk()) return Status::Ok;
    return std::get<ErrorInfo>(state_).status;
  }

  std::string_view Message() const noexcept {
    if (IsOk()) return {};
    return std::get<ErrorInfo>(state_).message;
  }

  /// Requires !IsOk(). The returned reference is owned by this Result.
  const std::error_code& Error() const {
    assert(!IsOk());
    return std::get<ErrorInfo>(state_).cause;
  }

  template <typename U>
  static Result ErrFrom(const Result<U>& source) {
    assert(!source.IsOk());
    if (source.IsOk()) return Err(Status::Error);
    const auto& info = std::get<ErrorInfo>(source.state_);
    return Result(info);
  }

 private:
  explicit Result(std::monostate state) : state_(state) {}
  explicit Result(ErrorInfo info) : state_(std::move(info)) {}

  template <typename>
  friend class Result;
  std::variant<std::monostate, ErrorInfo> state_;
};

}  // namespace sitos

#endif  // SITOS_RESULT_HPP
