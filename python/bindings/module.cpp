// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <span>

#include <nanobind/nanobind.h>

#include "param_value_conversion.hpp"

#if SITOS_PYTHON_WITH_ZENOH
#include "transport/zenoh_runtime_anchor.hpp"
#endif

namespace nb = nanobind;
using namespace nb::literals;

namespace {

nb::bytes EncodeValue(const nb::handle& value) {
  const auto encoded = sitos::python::detail::ParamValueFromPython(value).Encode();
  return nb::bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

nb::object DecodeValue(const nb::bytes& payload) {
  const auto* data = static_cast<const std::byte*>(payload.data());
  const auto decoded = sitos::ParamValue::Decode(std::span<const std::byte>(data, payload.size()));
  if (!decoded.has_value()) {
    throw nb::value_error("invalid payload-v1 value");
  }
  return sitos::python::detail::ParamValueToPython(*decoded);
}

}  // namespace

NB_MODULE(_sitos, module) {
#if SITOS_PYTHON_WITH_ZENOH
  // Retain the deliberate Zenoh-enabled wheel runtime edge without exposing its C API here.
  static_cast<void>(sitos::detail::ZenohRuntimeAnchor());
#endif
  module.doc() = "sitos payload-v1 conversion helpers";
  module.attr("__version__") = SITOS_PYTHON_VERSION;
  module.def("encode_value", &EncodeValue, "value"_a);
  module.def("decode_value", &DecodeValue, "payload"_a);
}
