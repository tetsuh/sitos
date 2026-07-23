// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "client_binding.hpp"
#include "numpy_binding.hpp"
#include "param_value_conversion.hpp"
#include "sitos/batch.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {
class PyParamCache {
 public:
  explicit PyParamCache(const std::string& prefix, const nb::object& json,
                        const nb::handle& timeout) {
    ClientConfig config;
    config.prefix = prefix;
    config.query_timeout = std::chrono::milliseconds(GetTimeout(timeout));
    if (!json.is_none()) config.zenoh_config_json = nb::cast<std::string>(json);
    auto result = [&config] {
      nb::gil_scoped_release release;
      return ParamCache::Open(std::move(config));
    }();
    std::optional<ParamCache> native{Take(std::move(result))};
    try {
      state_->native = std::make_shared<ParamCache>(std::move(*native));
    } catch (...) {
      nb::gil_scoped_release release;
      native.reset();
      throw;
    }
    nb::gil_scoped_release release;
    native.reset();
  }

  ~PyParamCache() { Close(); }

  void Close() noexcept {
    auto state = state_;
    nb::gil_scoped_release release;
    std::shared_ptr<ParamCache> native;
    {
      std::unique_lock lock(state->mutex);
      if (state->phase == Phase::Closed) return;
      if (state->phase == Phase::Closing) {
        state->condition.wait(lock, [&state] { return state->phase == Phase::Closed; });
        return;
      }
      state->phase = Phase::Closing;
      state->condition.wait(lock, [&state] { return state->in_flight == 0; });
      native = std::move(state->native);
    }
    if (native) {
      native->Detach();
      native.reset();
    }
    {
      std::lock_guard lock(state->mutex);
      state->phase = Phase::Closed;
    }
    state->condition.notify_all();
  }

  PyParamCache& Enter() {
    static_cast<void>(Acquire());
    return *this;
  }

  bool Exit(const nb::object&, const nb::object&, const nb::object&) {
    Close();
    return false;
  }

  void Attach(const nb::handle& sid_input) {
    auto lease = Acquire();
    const auto sid = nb::cast<std::string>(sid_input);
    auto result =
        InvokeNative(std::move(lease), [&sid](ParamCache& cache) { return cache.Attach(sid); });
    Take(result);
  }

  void Detach() noexcept {
    auto lease = AcquireForDetach();
    if (!lease.has_value()) return;
    nb::gil_scoped_release release;
    lease->Native().Detach();
  }

  void Put(const nb::handle& key_input, const nb::handle& value) {
    auto lease = Acquire();
    const auto key = nb::cast<std::string>(key_input);
    const auto converted = ParamValueFromPython(value);
    auto result = InvokeNative(std::move(lease), [&key, &converted](ParamCache& cache) {
      return cache.Put(key, converted);
    });
    Take(result);
  }

  void PutBatch(const nb::handle& entries) {
    auto lease = Acquire();
    auto materialized = MaterializeBatchEntries(entries);
    auto result = InvokeNative(std::move(lease), [&materialized](ParamCache& cache) {
      return cache.PutBatch(materialized);
    });
    Take(result);
  }

  nb::object GetArray(const nb::handle& key_input, const nb::handle& dtype_input) {
    auto lease = Acquire();
    const auto key = nb::cast<std::string>(key_input);
    auto result = lease.Native().GetSpan<std::byte>(key);
    if (!result.IsOk()) Take(std::move(result));
    return MakeReadonlyNumpyArray(result.Value(), nb::borrow<nb::object>(dtype_input));
  }

  nb::object Get(const nb::handle& key_input, const nb::object& default_value,
                 const nb::object& missing, const nb::object& type) {
    auto lease = Acquire();
    const auto key = nb::cast<std::string>(key_input);
    auto result = lease.Native().GetShared(key);
    if (!result.IsOk()) {
      if (result.StatusCode() == Status::NotFound && default_value.ptr() != missing.ptr()) {
        return default_value;
      }
      Take(std::move(result));
    }
    auto shared = std::move(result).Value();
    if (type.is_none()) return ParamValueToPython(*shared);
    return ParamValueToPython(ConvertTyped(*shared, type));
  }

  bool Contains(const nb::handle& key_input) {
    auto lease = Acquire();
    const auto key = nb::cast<std::string>(key_input);
    return Take(lease.Native().Contains(key));
  }

  nb::object Items(const nb::handle& prefix_input) {
    auto lease = Acquire();
    const auto prefix = nb::cast<std::string>(prefix_input);
    std::vector<std::pair<std::string, ParamValue>> values;
    auto result =
        lease.Native().List(prefix, [&values](std::string_view key, const ParamValue& value) {
          values.emplace_back(key, value);
          return true;
        });
    Take(result);
    nb::list rows;
    for (auto& [key, value] : values) {
      rows.append(nb::make_tuple(key, ParamValueToPython(value)));
    }
    return rows.attr("__iter__")();
  }

 private:
  enum class Phase { Open, Closing, Closed };

  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    Phase phase = Phase::Open;
    std::size_t in_flight = 0;
    std::shared_ptr<ParamCache> native;
  };

  class OperationLease {
   public:
    OperationLease(std::shared_ptr<State> state, std::shared_ptr<ParamCache> native)
        : state_(std::move(state)), native_(std::move(native)) {}
    ~OperationLease() { Release(); }
    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;
    OperationLease(OperationLease&& other) noexcept
        : state_(std::move(other.state_)), native_(std::move(other.native_)) {}
    OperationLease& operator=(OperationLease&& other) noexcept {
      if (this != &other) {
        Release();
        state_ = std::move(other.state_);
        native_ = std::move(other.native_);
      }
      return *this;
    }

    ParamCache& Native() const { return *native_; }

   private:
    void Release() noexcept {
      if (!state_) return;
      native_.reset();
      {
        std::lock_guard lock(state_->mutex);
        assert(state_->in_flight > 0);
        if (state_->in_flight > 0) --state_->in_flight;
      }
      state_->condition.notify_all();
      state_.reset();
    }

    std::shared_ptr<State> state_;
    std::shared_ptr<ParamCache> native_;
  };

  OperationLease Acquire() const {
    auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->phase != Phase::Open || !state->native) {
      throw nb::value_error("ParamCache is closed");
    }
    ++state->in_flight;
    return OperationLease(state, state->native);
  }

  std::optional<OperationLease> AcquireForDetach() const noexcept {
    auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->phase != Phase::Open || !state->native) return std::nullopt;
    ++state->in_flight;
    return std::optional<OperationLease>(std::in_place, state, state->native);
  }

  template <typename Operation>
  static auto InvokeNative(OperationLease lease, Operation&& operation)
      -> std::invoke_result_t<Operation, ParamCache&> {
    nb::gil_scoped_release release;
    return std::forward<Operation>(operation)(lease.Native());
  }

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace sitos::python::detail

void BindParamCache(nb::module_& python_module) {
  using sitos::python::detail::PyParamCache;
  nb::object missing = nb::dict();
  python_module.attr("_PARAM_CACHE_MISSING") = missing;
  nb::class_<PyParamCache>(python_module, "ParamCache")
      .def(nb::init<const std::string&, const nb::object&, const nb::handle>(), nb::kw_only(),
           "prefix"_a = "sitos", "zenoh_config_json"_a = nb::none(), "query_timeout_ms"_a = 5000)
      .def("close", &PyParamCache::Close)
      .def("__enter__", &PyParamCache::Enter, nb::rv_policy::reference_internal)
      .def("__exit__", &PyParamCache::Exit, "exc_type"_a.none(), "exc_value"_a.none(),
           "traceback"_a.none())
      .def("attach", &PyParamCache::Attach, "sid"_a)
      .def("detach", &PyParamCache::Detach)
      .def("put", &PyParamCache::Put, "key"_a, "value"_a)
      .def("put_batch", &PyParamCache::PutBatch, "entries"_a)
      .def(
          "get",
          [missing](PyParamCache& self, const nb::handle& key, nb::object default_value,
                    nb::object type) { return self.Get(key, default_value, missing, type); },
          "key"_a, "default"_a.none() = missing, nb::kw_only(), "type"_a.none() = nb::none())
      .def("contains", &PyParamCache::Contains, "key"_a)
      .def("items", &PyParamCache::Items, "prefix"_a = "")
      .def("get_array", &PyParamCache::GetArray, "key"_a, nb::kw_only(), "dtype"_a);
}
