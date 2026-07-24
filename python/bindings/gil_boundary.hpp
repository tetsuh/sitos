// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_GIL_BOUNDARY_HPP_
#define SITOS_PYTHON_BINDINGS_GIL_BOUNDARY_HPP_

#include <nanobind/nanobind.h>

#include <chrono>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sitos::python::detail {

enum class GilBoundary { Constructor, CreateSession, Stop };

void WaitAtGilBoundary(GilBoundary boundary);
void ArmGilBoundary(std::string_view boundary);
bool WaitForGilBoundary(std::string_view boundary, std::chrono::milliseconds timeout);
void ReleaseGilBoundary(std::string_view boundary);
void ResetGilBoundary();

/// Runs one native operation with the Python GIL released.
template <typename Function>
auto InvokeReleased(GilBoundary boundary, Function&& function)
    -> std::invoke_result_t<Function> {
  nanobind::gil_scoped_release release;
  WaitAtGilBoundary(boundary);
  return std::forward<Function>(function)();
}

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_GIL_BOUNDARY_HPP_
