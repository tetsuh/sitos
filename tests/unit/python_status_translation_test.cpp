// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "python/bindings/status_translation.hpp"

#include <gtest/gtest.h>

namespace {

using sitos::Status;
using sitos::python::detail::PythonErrorKind;
using sitos::python::detail::StatusToPythonError;

TEST(PythonStatusTranslationTest, MapsEveryStatusDeterministically) {
  EXPECT_EQ(StatusToPythonError(Status::Ok), PythonErrorKind::kSitosError);
  EXPECT_EQ(StatusToPythonError(Status::NotFound), PythonErrorKind::kNotFound);
  EXPECT_EQ(StatusToPythonError(Status::TypeMismatch), PythonErrorKind::kTypeMismatch);
  EXPECT_EQ(StatusToPythonError(Status::Timeout), PythonErrorKind::kTimeout);
  EXPECT_EQ(StatusToPythonError(Status::Disconnected), PythonErrorKind::kDisconnected);
  EXPECT_EQ(StatusToPythonError(Status::ReadOnly), PythonErrorKind::kReadOnly);
  EXPECT_EQ(StatusToPythonError(Status::InvalidKey), PythonErrorKind::kValueError);
  EXPECT_EQ(StatusToPythonError(Status::InvalidArgument), PythonErrorKind::kValueError);
  EXPECT_EQ(StatusToPythonError(Status::Error), PythonErrorKind::kSitosError);
}

}  // namespace
