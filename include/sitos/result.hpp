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

inline ErrorInfo MakeErrorInfo(Status status, std::string message, std::error_code cause) {
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

template <typename State>
bool ResultIsOk(const State& state) noexcept {
  return state.index() == 0;
}

template <typename State>
Status ResultStatus(const State& state) noexcept {
  if (state.valueless_by_exception()) return Status::Error;
  if (ResultIsOk(state)) return Status::Ok;
  return std::get<1>(state).status;
}

template <typename State>
std::string_view ResultMessage(const State& state) noexcept {
  if (state.valueless_by_exception() || ResultIsOk(state)) return {};
  return std::get<1>(state).message;
}

template <typename State>
const ErrorInfo& ResultError(const State& state) {
  assert(state.index() == 1);
  return std::get<1>(state);
}

template <typename State, typename Source>
void AssignResultState(State& target, Source&& source) {
  try {
    target = std::forward<Source>(source);
  } catch (...) {
    if (target.valueless_by_exception()) {
      target.template emplace<1>(MakeErrorInfo(Status::Error, {}, {}));
    }
    throw;
  }
}

}  // namespace detail

/// A move-friendly, exclusive success-or-error result.
template <typename T>
class Result {
 public:
  static Result Ok(T value) { return Result(std::move(value)); }

  static Result Err(std::error_code cause) { return Result(detail::MakeLegacyErrorInfo(cause)); }

  static Result Err(Status status, std::string message = {}, std::error_code cause = {}) {
    return Result(detail::MakeErrorInfo(status, std::move(message), cause));
  }

  Result(const Result&) = default;
  Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;

  Result& operator=(const Result& other)
    requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
  {
    if (this != &other) detail::AssignResultState(state_, other.state_);
    return *this;
  }

  Result& operator=(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                             std::is_nothrow_move_assignable_v<T>)
    requires(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>)
  {
    if (this != &other) detail::AssignResultState(state_, std::move(other.state_));
    return *this;
  }

  bool IsOk() const noexcept { return detail::ResultIsOk(state_); }
  explicit operator bool() const noexcept { return IsOk(); }

  /// Returns Status::Ok and an empty view for a successful result.
  Status StatusCode() const noexcept { return detail::ResultStatus(state_); }

  /// Returns a view that remains valid only while this Result's error state lives.
  /// The view is invalidated by Result assignment or move.
  std::string_view Message() const noexcept { return detail::ResultMessage(state_); }

  /// Requires IsOk().
  const T& Value() const& {
    assert(IsOk());
    return std::get<0>(state_);
  }
  /// Requires IsOk().
  T& Value() & {
    assert(IsOk());
    return std::get<0>(state_);
  }
  /// Requires IsOk().
  T&& Value() && {
    assert(IsOk());
    return std::move(std::get<0>(state_));
  }

  /// Requires !IsOk(). The returned reference is owned by this Result.
  const std::error_code& Error() const { return detail::ResultError(state_).cause; }

  /// Propagates an error without accessing or copying source's value.
  template <typename U>
  static Result ErrFrom(const Result<U>& source) {
    assert(!source.IsOk());
    if (source.IsOk()) return Err(Status::Error);
    return Result(detail::ResultError(source.state_));
  }

 private:
  explicit Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
  explicit Result(ErrorInfo info) : state_(std::in_place_index<1>, std::move(info)) {}

  template <typename>
  friend class Result;
  std::variant<T, ErrorInfo> state_;
};

template <>
class Result<void> {
 public:
  static Result Ok() { return Result(std::monostate{}); }

  static Result Err(std::error_code cause) { return Result(detail::MakeLegacyErrorInfo(cause)); }

  static Result Err(Status status, std::string message = {}, std::error_code cause = {}) {
    return Result(detail::MakeErrorInfo(status, std::move(message), cause));
  }

  bool IsOk() const noexcept { return detail::ResultIsOk(state_); }
  explicit operator bool() const noexcept { return IsOk(); }

  Status StatusCode() const noexcept { return detail::ResultStatus(state_); }

  /// Returns a view that remains valid only while this Result's error state lives.
  /// The view is invalidated by Result assignment or move.
  std::string_view Message() const noexcept { return detail::ResultMessage(state_); }

  /// Requires !IsOk(). The returned reference is owned by this Result.
  const std::error_code& Error() const { return detail::ResultError(state_).cause; }

  template <typename U>
  static Result ErrFrom(const Result<U>& source) {
    assert(!source.IsOk());
    if (source.IsOk()) return Err(Status::Error);
    return Result(detail::ResultError(source.state_));
  }

 private:
  explicit Result(std::monostate state) : state_(std::in_place_index<0>, state) {}
  explicit Result(ErrorInfo info) : state_(std::in_place_index<1>, std::move(info)) {}

  template <typename>
  friend class Result;
  std::variant<std::monostate, ErrorInfo> state_;
};

}  // namespace sitos

#endif  // SITOS_RESULT_HPP
