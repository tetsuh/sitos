// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_CLIENT_BINDING_HPP_
#define SITOS_PYTHON_BINDINGS_CLIENT_BINDING_HPP_

#include <nanobind/nanobind.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "sitos/param_value.hpp"
#include "sitos/result.hpp"

namespace sitos::python::detail {

class SitosError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
class NotFoundError : public SitosError {
 public:
  using SitosError::SitosError;
};
class TypeMismatchError : public SitosError {
 public:
  using SitosError::SitosError;
};
class TimeoutError : public SitosError {
 public:
  using SitosError::SitosError;
};
class DisconnectedError : public SitosError {
 public:
  using SitosError::SitosError;
};
class ReadOnlyError : public SitosError {
 public:
  using SitosError::SitosError;
};

void RegisterClientExceptions(nanobind::module_& python_module);
[[noreturn]] void ThrowStatus(Status status, std::string_view message);

template <typename T>
T Take(Result<T>&& result) {
  if (!result.IsOk()) ThrowStatus(result.StatusCode(), result.Message());
  return std::move(result).Value();
}

inline void Take(Result<void>&& result) {
  if (!result.IsOk()) ThrowStatus(result.StatusCode(), result.Message());
}

std::int64_t GetTimeout(const nanobind::handle& value);
ParamValue ConvertTyped(const ParamValue& value, const nanobind::object& type);

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_CLIENT_BINDING_HPP_
