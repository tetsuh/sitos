// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_STATUS_TRANSLATION_HPP_
#define SITOS_PYTHON_BINDINGS_STATUS_TRANSLATION_HPP_

#include "sitos/status.hpp"

namespace sitos::python::detail {

enum class PythonErrorKind {
  kSitosError,
  kNotFound,
  kTypeMismatch,
  kTimeout,
  kDisconnected,
  kReadOnly,
  kValueError,
};

constexpr PythonErrorKind StatusToPythonError(sitos::Status status) {
  switch (status) {
    case sitos::Status::InvalidKey:
    case sitos::Status::InvalidArgument:
      return PythonErrorKind::kValueError;
    case sitos::Status::NotFound:
      return PythonErrorKind::kNotFound;
    case sitos::Status::TypeMismatch:
      return PythonErrorKind::kTypeMismatch;
    case sitos::Status::Timeout:
      return PythonErrorKind::kTimeout;
    case sitos::Status::Disconnected:
      return PythonErrorKind::kDisconnected;
    case sitos::Status::ReadOnly:
      return PythonErrorKind::kReadOnly;
    case sitos::Status::Error:
    case sitos::Status::Ok:
      return PythonErrorKind::kSitosError;
  }
  return PythonErrorKind::kSitosError;
}

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_STATUS_TRANSLATION_HPP_
