// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_NUMPY_BINDING_HPP_
#define SITOS_PYTHON_BINDINGS_NUMPY_BINDING_HPP_

#include <nanobind/nanobind.h>

#include "sitos/param_cache.hpp"

namespace sitos::python::detail {

ParamValue ParamValueFromNumpy(const nanobind::handle& value);
nanobind::object MakeReadonlyNumpyArray(const SpanHandle<std::byte>& value,
                                        const nanobind::object& dtype);

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_NUMPY_BINDING_HPP_
