// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "param_value_conversion.hpp"
#include "status_translation.hpp"
#include "sitos/param_store.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {

class SitosError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
class NotFoundError : public SitosError { using SitosError::SitosError; };
class TypeMismatchError : public SitosError { using SitosError::SitosError; };
class TimeoutError : public SitosError { using SitosError::SitosError; };
class DisconnectedError : public SitosError { using SitosError::SitosError; };
class ReadOnlyError : public SitosError { using SitosError::SitosError; };

[[noreturn]] void ThrowStatus(sitos::Status status, std::string_view message) {
  const std::string fallback = sitos::MakeErrorCode(status).message();
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

template <typename T>
T Take(sitos::Result<T>&& result) {
  if (!result.IsOk()) ThrowStatus(result.StatusCode(), result.Message());
  return std::move(result).Value();
}

void Take(sitos::Result<void>&& result) {
  if (!result.IsOk()) ThrowStatus(result.StatusCode(), result.Message());
}

std::int64_t GetTimeout(const nb::handle& value) {
  if (!nb::isinstance<nb::int_>(value) || nb::isinstance<nb::bool_>(value)) {
    throw nb::type_error("query_timeout_ms must be a positive int");
  }
  std::int64_t converted = 0;
  try {
    converted = nb::cast<std::int64_t>(value);
  } catch (const std::exception&) {
    throw nb::value_error("query_timeout_ms is outside the C++ millisecond range");
  }
  if (converted <= 0) throw nb::value_error("query_timeout_ms must be positive");
  return converted;
}

sitos::ParamValue ConvertTyped(const sitos::ParamValue& value, const nb::object& type) {
  if (type.is_none()) return value;
  const auto builtins = nb::module_::import_("builtins");
  if (type.ptr() == builtins.attr("bool").ptr()) {
    auto converted = value.As<bool>();
    if (!converted) ThrowStatus(sitos::Status::TypeMismatch, "parameter value type mismatch");
    return sitos::ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("int").ptr()) {
    auto converted = value.As<std::int64_t>();
    if (!converted) ThrowStatus(sitos::Status::TypeMismatch, "parameter value type mismatch");
    return sitos::ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("float").ptr()) {
    auto converted = value.As<double>();
    if (!converted) ThrowStatus(sitos::Status::TypeMismatch, "parameter value type mismatch");
    return sitos::ParamValue(*converted);
  }
  if (type.ptr() == builtins.attr("str").ptr()) {
    auto converted = value.As<std::string>();
    if (!converted) ThrowStatus(sitos::Status::TypeMismatch, "parameter value type mismatch");
    return sitos::ParamValue(std::move(*converted));
  }
  if (type.ptr() == builtins.attr("bytes").ptr()) {
    auto converted = value.As<std::vector<std::byte>>();
    if (!converted) ThrowStatus(sitos::Status::TypeMismatch, "parameter value type mismatch");
    return sitos::ParamValue(std::move(*converted));
  }
  throw nb::type_error("type must be None, bool, int, float, str, or bytes");
}

class PyParamStore {
 public:
  explicit PyParamStore(const std::string& prefix, const nb::object& json,
                        const nb::handle& timeout) {
    sitos::ClientConfig config;
    config.prefix = prefix;
    config.query_timeout = std::chrono::milliseconds(GetTimeout(timeout));
    if (!json.is_none()) config.zenoh_config_json = nb::cast<std::string>(json);
    auto result = [&] {
      nb::gil_scoped_release release;
      return sitos::ParamStore::Open(std::move(config));
    }();
    std::optional<sitos::ParamStore> native{Take(std::move(result))};
    try {
      state_->native = std::make_shared<sitos::ParamStore>(std::move(*native));
    } catch (...) {
      nb::gil_scoped_release release;
      native.reset();
      throw;
    }
    nb::gil_scoped_release release;
    native.reset();
  }

  ~PyParamStore() { Close(); }

  void Close() noexcept {
    std::shared_ptr<sitos::ParamStore> native;
    {
      std::lock_guard lock(state_->mutex);
      native.swap(state_->native);
    }
    if (native) {
      nb::gil_scoped_release release;
      native.reset();
    }
  }

  PyParamStore& Enter() {
    ReleaseNative(Acquire());
    return *this;
  }

  bool Exit(const nb::object&, const nb::object&, const nb::object&) {
    Close();
    return false;
  }

  void Put(const std::string& scope, const std::string& key, const nb::handle& value) {
    auto native = Acquire();
    try {
      auto converted = ParamValueFromPython(value);
      auto result = InvokeNative(std::move(native), [&](sitos::ParamStore& store) {
        return store.Put(scope, key, converted);
      });
      Take(std::move(result));
    } catch (...) {
      ReleaseNative(std::move(native));
      throw;
    }
  }

  void PutBatch(const std::string& scope, const nb::handle& entries) {
    auto native = Acquire();
    try {
      std::vector<sitos::BatchEntry> materialized;
      if (nb::hasattr(entries, "items")) {
        nb::object items = entries.attr("items")();
        for (nb::handle item : nb::iter(items)) {
          nb::tuple pair = nb::borrow<nb::tuple>(item);
          materialized.push_back({nb::cast<std::string>(pair[0]),
                                  ParamValueFromPython(pair[1])});
        }
      } else {
        for (auto item : nb::iter(entries)) {
          if (!nb::isinstance<nb::tuple>(item) && !nb::isinstance<nb::list>(item)) {
            throw nb::type_error("put_batch entries must be two-item pairs");
          }
          if (nb::len(item) != 2) {
            throw nb::value_error("put_batch entries must have two items");
          }
          materialized.push_back({nb::cast<std::string>(item[0]),
                                  ParamValueFromPython(item[1])});
        }
      }
      auto result = InvokeNative(std::move(native), [&](sitos::ParamStore& store) {
        return store.PutBatch(scope, materialized);
      });
      Take(std::move(result));
    } catch (...) {
      ReleaseNative(std::move(native));
      throw;
    }
  }

  void Delete(const std::string& scope, const std::string& key) {
    auto result = InvokeNative(Acquire(), [&](sitos::ParamStore& store) {
      return store.Delete(scope, key);
    });
    Take(std::move(result));
  }

  nb::object Get(const std::string& scope, const std::string& key, const nb::object& default_value,
                 const nb::object& missing, const nb::object& type) {
    auto result = InvokeNative(Acquire(), [&](sitos::ParamStore& store) {
      return store.Get(scope, key);
    });
    if (!result.IsOk()) {
      if (result.StatusCode() == sitos::Status::NotFound && default_value.ptr() != missing.ptr()) {
        return default_value;
      }
      Take(std::move(result));
    }
    auto converted = ConvertTyped(result.Value(), type);
    return ParamValueToPython(converted);
  }

  bool Contains(const std::string& scope, const std::string& key) {
    auto result = InvokeNative(Acquire(), [&](sitos::ParamStore& store) {
      return store.Contains(scope, key);
    });
    return Take(std::move(result));
  }

  nb::object List(const std::string& scope, const std::string& prefix) {
    std::vector<std::pair<std::string, sitos::ParamValue>> values;
    auto native_result = InvokeNative(Acquire(), [&](sitos::ParamStore& store) {
      return store.List(scope, prefix, [&](std::string_view key,
                                           const sitos::ParamValue& value) {
        values.emplace_back(key, value);
        return true;
      });
    });
    Take(std::move(native_result));
    nb::list result;
    for (auto& entry : values) {
      result.append(nb::make_tuple(entry.first, ParamValueToPython(entry.second)));
    }
    return result.attr("__iter__")();
  }

 private:
  static void ReleaseNative(std::shared_ptr<sitos::ParamStore> native) noexcept {
    if (!native) return;
    nb::gil_scoped_release release;
    native.reset();
  }

  template <typename Operation>
  static auto InvokeNative(std::shared_ptr<sitos::ParamStore> native, Operation&& operation) {
    nb::gil_scoped_release release;
    try {
      auto result = std::forward<Operation>(operation)(*native);
      native.reset();
      return result;
    } catch (...) {
      native.reset();
      throw;
    }
  }

  std::shared_ptr<sitos::ParamStore> Acquire() const {
    std::lock_guard lock(state_->mutex);
    if (!state_->native) throw nb::value_error("ParamStore is closed");
    return state_->native;
  }

  struct State {
    mutable std::mutex mutex;
    std::shared_ptr<sitos::ParamStore> native;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace sitos::python::detail

void BindParamStore(nb::module_& module) {
  using namespace sitos::python::detail;
  static nb::exception<SitosError> base(module, "SitosError", PyExc_RuntimeError);
  static nb::exception<NotFoundError> not_found(module, "NotFoundError", base);
  static nb::exception<TypeMismatchError> mismatch(module, "TypeMismatchError", base);
  static nb::exception<TimeoutError> timeout(module, "TimeoutError", base);
  static nb::exception<DisconnectedError> disconnected(module, "DisconnectedError", base);
  static nb::exception<ReadOnlyError> readonly(module, "ReadOnlyError", base);
  module.attr("SitosError") = base;
  module.attr("NotFoundError") = not_found;
  module.attr("TypeMismatchError") = mismatch;
  module.attr("TimeoutError") = timeout;
  module.attr("DisconnectedError") = disconnected;
  module.attr("ReadOnlyError") = readonly;

  nb::object missing = nb::dict();
  module.attr("_PARAM_STORE_MISSING") = missing;
  nb::class_<PyParamStore>(module, "ParamStore")
      .def(nb::init<const std::string&, const nb::object&, const nb::handle>(), nb::kw_only(),
           "prefix"_a = "sitos", "zenoh_config_json"_a = nb::none(),
           "query_timeout_ms"_a = 5000)
      .def("close", &PyParamStore::Close)
      .def("__enter__", &PyParamStore::Enter, nb::rv_policy::reference_internal)
      .def("__exit__", &PyParamStore::Exit, "exc_type"_a.none(), "exc_value"_a.none(),
           "traceback"_a.none())
      .def("put", &PyParamStore::Put, "scope"_a, "key"_a, "value"_a)
      .def("put_batch", &PyParamStore::PutBatch, "scope"_a, "entries"_a)
      .def("delete", &PyParamStore::Delete, "scope"_a, "key"_a)
      .def("get", [missing](PyParamStore& self, const std::string& scope, const std::string& key,
                             nb::object default_value, nb::object type) {
        return self.Get(scope, key, default_value, missing, type);
      },
           "scope"_a, "key"_a, "default"_a.none() = missing, nb::kw_only(),
           "type"_a.none() = nb::none())
      .def("contains", &PyParamStore::Contains, "scope"_a, "key"_a)
      .def("list", &PyParamStore::List, "scope"_a, "prefix"_a);
}
