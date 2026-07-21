// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_PARAM_VALUE_CONVERSION_HPP_
#define SITOS_PYTHON_BINDINGS_PARAM_VALUE_CONVERSION_HPP_

#include <nanobind/nanobind.h>

#include "sitos/param_value.hpp"

namespace sitos::python::detail {

ParamValue ParamValueFromPython(const nanobind::handle& value);
nanobind::object ParamValueToPython(const ParamValue& value);

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_PARAM_VALUE_CONVERSION_HPP_
