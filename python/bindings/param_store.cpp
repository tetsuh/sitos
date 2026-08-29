// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_store.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "client_binding.hpp"
#include "gil_boundary.hpp"
#include "param_value_conversion.hpp"

#if SITOS_PYTHON_TEST_SUPPORT
#include "sitos/ack.hpp"
#endif

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {

#if SITOS_PYTHON_TEST_SUPPORT
class PythonTestTransport final : public sitos::Transport {
 public:
  explicit PythonTestTransport(std::string mode) : mode_(std::move(mode)) {
    if (mode_ != "timeout" && mode_ != "delayed" && mode_ != "outcome_unknown" &&
        mode_ != "disconnected" && mode_ != "submit_disconnect") {
      throw std::invalid_argument("unknown ParamStore test mode: " + mode_);
    }
  }

  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding encoding,
                          sitos::PutOptions) override {
    kind_ = encoding.id == sitos::Encoding::kSitosV1Batch ? sitos::AckOperationKind::Batch
                                                          : sitos::AckOperationKind::Put;
    if (mode_ == "submit_disconnect") {
      return sitos::Result<void>::Err(sitos::Status::Disconnected, "test submission disconnected",
                                      std::make_error_code(std::errc::connection_reset));
    }
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    if (mode_ == "timeout") {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds{20}));
      return sitos::Result<void>::Ok();
    }
    if (mode_ == "delayed") {
      // InvokeNative has released the GIL before entering Transport::Get. The
      // source-only boundary proves polling is already blocked at this point.
      WaitAtGilBoundary(GilBoundary::ParamStoreAck);
    }
    const auto status =
        mode_ == "outcome_unknown" ? sitos::Status::OutcomeUnknown : sitos::Status::Disconnected;
    sitos::AckResultV1 result{kind_, status, sitos::AckDurability::Applied, 0,
                              0,     0,      sitos::kAckNoFailedSequence,   "test remote " + mode_};
    if (mode_ == "delayed") {
      result.status = sitos::Status::Ok;
      result.applied_count = 1;
      result.failed_index = sitos::kAckNoFailedIndex;
      result.message = "test delayed acknowledgement";
    }
    auto encoded = sitos::EncodeAckResult(result);
    if (!encoded.IsOk()) return sitos::Result<void>::ErrFrom(encoded);
    const auto payload = std::move(encoded).Value();
    sink(keyexpr, payload, sitos::Encoding{std::string(sitos::Encoding::kSitosV1Ack)});
    return sitos::Result<void>::Ok();
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)>) override {
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

 private:
  std::string mode_;
  sitos::AckOperationKind kind_ = sitos::AckOperationKind::Put;
};
#endif

class PyParamStore {
 public:
#if SITOS_PYTHON_TEST_SUPPORT
  static PyParamStore TestStore(const std::string& mode) {
    auto transport = std::make_shared<PythonTestTransport>(mode);
    sitos::ClientConfig config;
    config.prefix = "sitos/python_test";
    config.query_timeout = std::chrono::milliseconds{100};
    auto result = sitos::ParamStore::Open(std::move(transport), std::move(config));
    return PyParamStore(std::make_shared<sitos::ParamStore>(Take(std::move(result))));
  }
#endif

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

  PyParamStore(const PyParamStore&) = delete;
  PyParamStore& operator=(const PyParamStore&) = delete;
  PyParamStore(PyParamStore&& other) noexcept : state_(std::move(other.state_)) {}
  PyParamStore& operator=(PyParamStore&& other) noexcept {
    if (this != &other) {
      Close();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  ~PyParamStore() { Close(); }

  void Close() noexcept {
    if (!state_) return;
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

  void Put(const std::string& scope, const std::string& key, const nb::handle& value,
           const nb::object& ack, const nb::object& timeout) {
    auto native = Acquire();
    try {
      if (!nb::isinstance<nb::bool_>(ack)) throw nb::type_error("ack must be a bool");
      const bool acknowledged = nb::cast<bool>(ack);
      const auto timeout_ms = GetWriteTimeout(timeout, acknowledged);
      auto converted = ParamValueFromPython(value);
      sitos::ParamStore::WriteOptions options{acknowledged, std::chrono::milliseconds(timeout_ms)};
      auto result = InvokeNative(std::move(native),
                                 [&scope, &key, &converted, options](sitos::ParamStore& store) {
                                   return store.Put(scope, key, converted, options);
                                 });
      Take(result);
    } catch (...) {
      ReleaseNative(std::move(native));
      throw;
    }
  }

  void PutBatch(const std::string& scope, const nb::handle& entries, const nb::object& ack,
                const nb::object& timeout) {
    auto native = Acquire();
    try {
      if (!nb::isinstance<nb::bool_>(ack)) throw nb::type_error("ack must be a bool");
      const bool acknowledged = nb::cast<bool>(ack);
      const auto timeout_ms = GetWriteTimeout(timeout, acknowledged);
      auto materialized = MaterializeBatchEntries(entries);
      sitos::ParamStore::WriteOptions options{acknowledged, std::chrono::milliseconds(timeout_ms)};
      auto result = InvokeNative(std::move(native),
                                 [&scope, &materialized, options](sitos::ParamStore& store) {
                                   return store.PutBatch(scope, materialized, options);
                                 });
      Take(result);
    } catch (...) {
      ReleaseNative(std::move(native));
      throw;
    }
  }

  void Delete(const std::string& scope, const std::string& key) {
    auto result = InvokeNative(
        Acquire(), [&scope, &key](sitos::ParamStore& store) { return store.Delete(scope, key); });
    Take(result);
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
    Take(native_result);
    nb::list result;
    for (auto& [key, value] : values) {
      result.append(nb::make_tuple(key, ParamValueToPython(value)));
    }
    return result.attr("__iter__")();
  }

 private:
#if SITOS_PYTHON_TEST_SUPPORT
  explicit PyParamStore(std::shared_ptr<sitos::ParamStore> native)
      : state_(std::make_shared<State>()) {
    state_->native = std::move(native);
  }
#endif

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
#if SITOS_PYTHON_TEST_SUPPORT
      .def_static("_test_store", &PyParamStore::TestStore, "mode"_a)
#endif
      .def("__enter__", &PyParamStore::Enter, nb::rv_policy::reference_internal)
      .def("__exit__", &PyParamStore::Exit, "exc_type"_a.none(), "exc_value"_a.none(),
           "traceback"_a.none())
      .def("put", &PyParamStore::Put, "scope"_a, "key"_a, "value"_a, nb::kw_only(), "ack"_a = true,
           "ack_timeout_ms"_a = 3000)
      .def("put_batch", &PyParamStore::PutBatch, "scope"_a, "entries"_a, nb::kw_only(),
           "ack"_a = true, "ack_timeout_ms"_a = 3000)
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
