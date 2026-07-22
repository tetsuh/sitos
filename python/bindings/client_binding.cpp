// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "client_binding.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "param_value_conversion.hpp"
#include "sitos/status.hpp"
#include "status_translation.hpp"

namespace nb = nanobind;

namespace sitos::python::detail {

void RegisterClientExceptions(nb::module_& python_module) {
  static nb::exception<SitosError> base(python_module, "SitosError", PyExc_RuntimeError);
  static nb::exception<NotFoundError> not_found(python_module, "NotFoundError", base);
  static nb::exception<TypeMismatchError> mismatch(python_module, "TypeMismatchError", base);
  static nb::exception<TimeoutError> timeout(python_module, "TimeoutError", base);
  static nb::exception<DisconnectedError> disconnected(python_module, "DisconnectedError", base);
  static nb::exception<ReadOnlyError> readonly(python_module, "ReadOnlyError", base);
  python_module.attr("SitosError") = base;
  python_module.attr("NotFoundError") = not_found;
  python_module.attr("TypeMismatchError") = mismatch;
  python_module.attr("TimeoutError") = timeout;
  python_module.attr("DisconnectedError") = disconnected;
  python_module.attr("ReadOnlyError") = readonly;
}

[[noreturn]] void ThrowStatus(Status status, std::string_view message) {
  const std::string fallback = MakeErrorCode(status).message();
  const std::string text = message.empty() ? fallback : std::string(message);
  switch (StatusToPythonError(status)) {
    case PythonErrorKind::kValueError:
      throw nb::value_error(text.c_str());
    case PythonErrorKind::kNotFound:
      throw NotFoundError(text);
    case PythonErrorKind::kTypeMismatch:
      throw TypeMismatchError(text);
    case PythonErrorKind::kTimeout:
      throw TimeoutError(text);
    case PythonErrorKind::kDisconnected:
      throw DisconnectedError(text);
    case PythonErrorKind::kReadOnly:
      throw ReadOnlyError(text);
    case PythonErrorKind::kSitosError:
      throw SitosError(text);
  }
  throw SitosError(text);
}

std::int64_t GetTimeout(const nb::handle& value) {
  if (!nb::isinstance<nb::int_>(value) || nb::isinstance<nb::bool_>(value)) {
    throw nb::type_error("query_timeout_ms must be a positive int");
  }
  std::int64_t converted = 0;
  try {
    converted = nb::cast<std::int64_t>(value);
  } catch (const nb::cast_error&) {
    throw nb::value_error("query_timeout_ms is outside the C++ millisecond range");
  }
  if (converted <= 0) throw nb::value_error("query_timeout_ms must be positive");
  return converted;
}

std::vector<BatchEntry> MaterializeBatchEntries(const nb::handle& entries) {
  std::vector<BatchEntry> materialized;
  const auto append_pair = [&materialized](const nb::handle& item) {
    if (!nb::isinstance<nb::tuple>(item) && !nb::isinstance<nb::list>(item)) {
      throw nb::type_error("put_batch entries must be two-item pairs");
    }
    if (nb::len(item) != 2) {
      throw nb::value_error("put_batch entries must have two items");
    }
    materialized.push_back({nb::cast<std::string>(item[0]), ParamValueFromPython(item[1])});
  };
  if (nb::hasattr(entries, "items")) {
    nb::object items = entries.attr("items")();
    for (nb::handle item : nb::iter(items)) append_pair(item);
  } else {
    for (nb::handle item : nb::iter(entries)) append_pair(item);
  }
  return materialized;
}

ParamValue ConvertTyped(const ParamValue& value, const nb::object& type) {
  if (type.is_none()) return value;
  const auto builtins = nb::module_::import_("builtins");
  if (type.ptr() == builtins.attr("bool").ptr()) {
    auto converted = value.As<bool>();
    if (!converted.has_value()) ThrowStatus(Status::TypeMismatch, "parameter value type mismatch");
    return ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("int").ptr()) {
    auto converted = value.As<std::int64_t>();
    if (!converted.has_value()) ThrowStatus(Status::TypeMismatch, "parameter value type mismatch");
    return ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("float").ptr()) {
    auto converted = value.As<double>();
    if (!converted.has_value()) ThrowStatus(Status::TypeMismatch, "parameter value type mismatch");
    return ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("str").ptr()) {
    auto converted = value.As<std::string>();
    if (!converted.has_value()) ThrowStatus(Status::TypeMismatch, "parameter value type mismatch");
    return ParamValue(std::move(*converted));
  }
  if (type.ptr() == builtins.attr("bytes").ptr()) {
    auto converted = value.As<std::vector<std::byte>>();
    if (!converted.has_value()) ThrowStatus(Status::TypeMismatch, "parameter value type mismatch");
    return ParamValue(std::move(*converted));
  }
  throw nb::type_error("type must be None, bool, int, float, str, or bytes");
}

}  // namespace sitos::python::detail
