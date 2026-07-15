// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_STATUS_HPP
#define SITOS_STATUS_HPP

#include <system_error>

namespace sitos {

/// Stable high-level classification for recoverable API failures.
enum class Status {
  Ok = 0,
  NotFound = 1,
  TypeMismatch = 2,
  Timeout = 3,
  Disconnected = 4,
  ReadOnly = 5,
  InvalidKey = 6,
  InvalidArgument = 7,
  Error = 8,
};

/// Returns the process-wide error category used by MakeErrorCode.
const std::error_category& StatusErrorCategory() noexcept;

/// Converts a non-success Status to a stable std::error_code.
/// Status::Ok is invalid and is normalized to Status::Error in release builds.
std::error_code MakeErrorCode(Status status);

}  // namespace sitos

#endif  // SITOS_STATUS_HPP
