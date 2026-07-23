// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "param_value_conversion.hpp"

#include <Python.h>
#include <nanobind/stl/string.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "numpy_api.hpp"
#include "numpy_binding.hpp"

namespace nb = nanobind;

namespace sitos::python::detail {

ParamValue ParamValueFromPython(const nb::handle& value) {
  if (PyArray_Check(value.ptr())) return ParamValueFromNumpy(value);
  if (nb::isinstance<nb::bool_>(value)) {
    return ParamValue(nb::cast<bool>(value));
  }
  if (nb::isinstance<nb::int_>(value)) {
    const auto integer = PyLong_AsLongLong(value.ptr());
    if (integer == -1 && PyErr_Occurred() != nullptr) {
      throw nb::python_error();
    }
    return ParamValue(static_cast<std::int64_t>(integer));
  }
  if (nb::isinstance<nb::float_>(value)) {
    return ParamValue(nb::cast<double>(value));
  }
  if (nb::isinstance<nb::str>(value)) {
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(value.ptr(), &size);
    if (data == nullptr) {
      throw nb::python_error();
    }
    return ParamValue(std::string(data, static_cast<std::size_t>(size)));
  }
  if (nb::isinstance<nb::bytes>(value)) {
    const auto bytes = nb::cast<nb::bytes>(value);
    const auto* data = static_cast<const char*>(bytes.data());
    std::vector<std::byte> body(bytes.size());
    std::memcpy(body.data(), data, bytes.size());
    return ParamValue(std::move(body));
  }
  throw nb::type_error(
      "encode_value accepts only bool, int, float, str, bytes, or supported numpy.ndarray");
}

nb::object ParamValueToPython(const ParamValue& value) {
  switch (value.type()) {
    case ValueType::Bool:
      return nb::cast(value.As<bool>().value());
    case ValueType::S64:
      return nb::cast(value.As<std::int64_t>().value());
    case ValueType::Dp:
      return nb::cast(value.As<double>().value());
    case ValueType::Str: {
      const auto string = value.As<std::string>().value();
      PyObject* unicode =
          PyUnicode_DecodeUTF8(string.data(), static_cast<Py_ssize_t>(string.size()), nullptr);
      if (unicode == nullptr) {
        throw nb::python_error();
      }
      return nb::steal<nb::object>(nb::handle(unicode));
    }
    case ValueType::Bytes: {
      const auto body = value.As<std::vector<std::byte>>().value();
      return nb::bytes(reinterpret_cast<const char*>(body.data()), body.size());
    }
  }
  throw nb::value_error("invalid payload-v1 value type");
}

}  // namespace sitos::python::detail
