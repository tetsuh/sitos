// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "client_binding.hpp"
#include "param_value_conversion.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {

class PyParamStore {
 public:
  explicit PyParamStore(const std::string& prefix, const nb::object& json,
                        const nb::handle& timeout) {
    sitos::ClientConfig config;
    config.prefix = prefix;
    config.query_timeout = std::chrono::milliseconds(GetTimeout(timeout));
    if (!json.is_none()) config.zenoh_config_json = nb::cast<std::string>(json);
    auto result = [&config] {
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
      auto result =
          InvokeNative(std::move(native), [&scope, &key, &converted](sitos::ParamStore& store) {
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
      auto result =
          InvokeNative(std::move(native), [&scope, &materialized](sitos::ParamStore& store) {
            return store.PutBatch(scope, materialized);
          });
      Take(std::move(result));
    } catch (...) {
      ReleaseNative(std::move(native));
      throw;
    }
  }

  void Delete(const std::string& scope, const std::string& key) {
    auto result = InvokeNative(
        Acquire(), [&scope, &key](sitos::ParamStore& store) { return store.Delete(scope, key); });
    Take(std::move(result));
  }

  nb::object Get(const std::string& scope, const std::string& key, const nb::object& default_value,
                 const nb::object& missing, const nb::object& type) {
    auto result = InvokeNative(
        Acquire(), [&scope, &key](sitos::ParamStore& store) { return store.Get(scope, key); });
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
    auto result = InvokeNative(
        Acquire(), [&scope, &key](sitos::ParamStore& store) { return store.Contains(scope, key); });
    return Take(std::move(result));
  }

  nb::object List(const std::string& scope, const std::string& prefix) {
    std::vector<std::pair<std::string, sitos::ParamValue>> values;
    auto native_result =
        InvokeNative(Acquire(), [&scope, &prefix, &values](sitos::ParamStore& store) {
          return store.List(scope, prefix,
                            [&values](std::string_view key, const sitos::ParamValue& value) {
                              values.emplace_back(key, value);
                              return true;
                            });
        });
    Take(std::move(native_result));
    nb::list result;
    for (auto& [key, value] : values) {
      result.append(nb::make_tuple(key, ParamValueToPython(value)));
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
  static auto InvokeNative(std::shared_ptr<sitos::ParamStore> native, Operation&& operation)
      -> std::invoke_result_t<Operation, sitos::ParamStore&> {
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

void BindParamStore(nb::module_& python_module) {
  using namespace sitos::python::detail;
  nb::object missing = nb::dict();
  python_module.attr("_PARAM_STORE_MISSING") = missing;
  nb::class_<PyParamStore>(python_module, "ParamStore")
      .def(nb::init<const std::string&, const nb::object&, const nb::handle>(), nb::kw_only(),
           "prefix"_a = "sitos", "zenoh_config_json"_a = nb::none(), "query_timeout_ms"_a = 5000)
      .def("close", &PyParamStore::Close)
      .def("__enter__", &PyParamStore::Enter, nb::rv_policy::reference_internal)
      .def("__exit__", &PyParamStore::Exit, "exc_type"_a.none(), "exc_value"_a.none(),
           "traceback"_a.none())
      .def("put", &PyParamStore::Put, "scope"_a, "key"_a, "value"_a)
      .def("put_batch", &PyParamStore::PutBatch, "scope"_a, "entries"_a)
      .def("delete", &PyParamStore::Delete, "scope"_a, "key"_a)
      .def(
          "get",
          [missing](PyParamStore& self, const std::string& scope, const std::string& key,
                    nb::object default_value,
                    nb::object type) { return self.Get(scope, key, default_value, missing, type); },
          "scope"_a, "key"_a, "default"_a.none() = missing, nb::kw_only(),
          "type"_a.none() = nb::none())
      .def("contains", &PyParamStore::Contains, "scope"_a, "key"_a)
      .def("list", &PyParamStore::List, "scope"_a, "prefix"_a);
}
