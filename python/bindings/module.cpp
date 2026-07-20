// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <Python.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "sitos/param_value.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

sitos::ParamValue ParamValueFromPython(const nb::handle& value) {
  if (nb::isinstance<nb::bool_>(value)) {
    return sitos::ParamValue(nb::cast<bool>(value));
  }
  if (nb::isinstance<nb::int_>(value)) {
    const auto integer = PyLong_AsLongLong(value.ptr());
    if (integer == -1 && PyErr_Occurred() != nullptr) {
      throw nb::python_error();
    }
    return sitos::ParamValue(static_cast<std::int64_t>(integer));
  }
  if (nb::isinstance<nb::float_>(value)) {
    return sitos::ParamValue(nb::cast<double>(value));
  }
  if (nb::isinstance<nb::str>(value)) {
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(value.ptr(), &size);
    if (data == nullptr) {
      throw nb::python_error();
    }
    return sitos::ParamValue(std::string(data, static_cast<std::size_t>(size)));
  }
  if (nb::isinstance<nb::bytes>(value)) {
    const auto bytes = nb::cast<nb::bytes>(value);
    const auto* data = static_cast<const char*>(bytes.data());
    std::vector<std::byte> body(bytes.size());
    std::memcpy(body.data(), data, bytes.size());
    return sitos::ParamValue(std::move(body));
  }
  throw nb::type_error("encode_value accepts only bool, int, float, str, or bytes");
}

nb::bytes EncodeValue(const nb::handle& value) {
  const auto encoded = ParamValueFromPython(value).Encode();
  return nb::bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

nb::object DecodeValue(const nb::bytes& payload) {
  const auto* data = static_cast<const std::byte*>(payload.data());
  const auto decoded = sitos::ParamValue::Decode(std::span<const std::byte>(data, payload.size()));
  if (!decoded.has_value()) {
    throw nb::value_error("invalid payload-v1 value");
  }

  const auto& value = *decoded;
  switch (value.type()) {
    case sitos::ValueType::Bool:
      return nb::cast(value.As<bool>().value());
    case sitos::ValueType::S64:
      return nb::cast(value.As<std::int64_t>().value());
    case sitos::ValueType::Dp:
      return nb::cast(value.As<double>().value());
    case sitos::ValueType::Str: {
      const auto string = value.As<std::string>().value();
      PyObject* unicode = PyUnicode_DecodeUTF8(
          string.data(), static_cast<Py_ssize_t>(string.size()), nullptr);
      if (unicode == nullptr) {
        throw nb::python_error();
      }
      return nb::steal<nb::object>(nb::handle(unicode));
    }
    case sitos::ValueType::Bytes: {
      const auto body = value.As<std::vector<std::byte>>().value();
      return nb::bytes(reinterpret_cast<const char*>(body.data()), body.size());
    }
  }
  throw nb::value_error("invalid payload-v1 value type");
}

}  // namespace

NB_MODULE(_sitos, module) {
  module.doc() = "sitos payload-v1 conversion helpers";
  module.attr("__version__") = SITOS_PYTHON_VERSION;
  module.def("encode_value", &EncodeValue, "value"_a);
  module.def("decode_value", &DecodeValue, "payload"_a);
}
