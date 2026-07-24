// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/session_view.hpp"
#include "sitos/storage_node.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "client_binding.hpp"
#include "gil_boundary.hpp"
#include "param_value_conversion.hpp"
#include "sitos/client_config.hpp"
#include "sitos/key.hpp"
#include "sitos/transport.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {

class PyInMemoryEngine {
 public:
  PyInMemoryEngine() : engine(std::make_shared<InMemoryEngine>()) {}

  std::shared_ptr<InMemoryEngine> engine;
};

class PyStorageNode;

class PySessionView {
 public:
  explicit PySessionView(SessionView view) : view_(std::move(view)) {}

  nb::object Get(const nb::handle& key_input, const nb::object& default_value,
                 const nb::object& missing, const nb::object& type) const {
    const auto key = nb::cast<std::string>(key_input);
    auto result = view_.Get(key);
    if (!result.IsOk()) {
      if (result.StatusCode() == Status::NotFound && default_value.ptr() != missing.ptr()) {
        return default_value;
      }
      Take(std::move(result));
    }
    if (type.is_none()) return ParamValueToPython(result.Value());
    return ParamValueToPython(ConvertTyped(result.Value(), type));
  }

  bool Contains(const nb::handle& key_input) const {
    const auto key = nb::cast<std::string>(key_input);
    return Take(view_.Contains(key));
  }

  nb::object Items(const nb::handle& prefix_input) const {
    const auto prefix = nb::cast<std::string>(prefix_input);
    std::vector<std::pair<std::string, ParamValue>> values;
    auto result = view_.List(prefix, [&values](std::string_view key, const ParamValue& value) {
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
  SessionView view_;
};

class PyStorageNode {
 public:
  PyStorageNode(PyInMemoryEngine& engine, const std::string& prefix, const nb::object& json) {
    ClientConfig config;
    config.prefix = prefix;
    if (!json.is_none()) config.zenoh_config_json = nb::cast<std::string>(json);
    auto config_result = ValidateClientConfig(config);
    Take(config_result);

    state_->engine = engine.engine;
    std::optional<std::string_view> config_json;
    if (config.zenoh_config_json.has_value()) config_json = *config.zenoh_config_json;

    StorageNodeConfig node_config;
    node_config.prefix = config.prefix;
    auto start_result = InvokeReleased(
        GilBoundary::Constructor,
        [this, config_json, node_config = std::move(node_config)]() mutable {
          auto transport_result = OpenZenohTransport(config_json);
          if (!transport_result.IsOk()) return Result<void>::ErrFrom(transport_result);
          state_->transport = std::move(transport_result).Value();
          state_->native.emplace(*state_->transport);
          return state_->native->Start(state_->engine, std::move(node_config));
        });
    try {
      Take(start_result);
    } catch (...) {
      nb::gil_scoped_release release;
      state_->native.reset();
      state_->transport.reset();
      state_->engine.reset();
      throw;
    }
  }

  ~PyStorageNode() { Stop(); }

  PyStorageNode(const PyStorageNode&) = delete;
  PyStorageNode& operator=(const PyStorageNode&) = delete;
  PyStorageNode(PyStorageNode&&) = delete;
  PyStorageNode& operator=(PyStorageNode&&) = delete;

  PyStorageNode& Enter() {
    static_cast<void>(Acquire());
    return *this;
  }

  bool Exit(const nb::object&, const nb::object&, const nb::object&) {
    Stop();
    return false;
  }

  void CreateSession(const nb::handle& sid_input) {
    auto lease = Acquire();
    const auto sid = nb::cast<std::string>(sid_input);
    auto result = InvokeReleased(GilBoundary::CreateSession,
                                 [&lease, &sid] { return lease.Native().CreateSession(sid); });
    Take(result);
  }

  void CloseSession(const nb::handle& sid_input) {
    auto lease = Acquire();
    const auto sid = nb::cast<std::string>(sid_input);
    if (!IsValidSessionId(sid)) throw nb::value_error("invalid session id");
    Take(lease.Native().CloseSession(sid));
  }

  std::vector<std::string> ActiveSessions() const {
    auto lease = TryAcquire();
    if (!lease.has_value()) return {};
    auto sessions = lease->Native().ActiveSessions();
    std::ranges::sort(sessions);
    return sessions;
  }

  PySessionView SessionViewFor(const nb::handle& sid_input) const {
    auto lease = AcquireDisconnected();
    const auto sid = nb::cast<std::string>(sid_input);
    auto result = sitos::SessionView::Open(lease.Native(), sid);
    return PySessionView(Take(std::move(result)));
  }

  void Stop() noexcept {
    auto state = state_;
    nb::gil_scoped_release release;
    std::unique_lock lock(state->mutex);
    if (state->phase == Phase::Stopped) return;
    if (state->phase == Phase::Stopping) {
      state->condition.wait(lock, [&state] { return state->phase == Phase::Stopped; });
      return;
    }
    state->phase = Phase::Stopping;
    state->condition.wait(lock, [&state] { return state->in_flight == 0; });
    lock.unlock();
    WaitAtGilBoundary(GilBoundary::Stop);
    state->native->Stop();
    state->native.reset();
    state->transport.reset();
    state->engine.reset();
    {
      std::lock_guard complete_lock(state->mutex);
      state->phase = Phase::Stopped;
    }
    state->condition.notify_all();
  }

 private:
  enum class Phase { Open, Stopping, Stopped };

  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    Phase phase = Phase::Open;
    std::size_t in_flight = 0;
    std::shared_ptr<InMemoryEngine> engine;
    std::unique_ptr<Transport> transport;
    std::optional<StorageNode> native;
  };

  class OperationLease {
   public:
    explicit OperationLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~OperationLease() { Release(); }
    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;
    OperationLease(OperationLease&& other) noexcept : state_(std::move(other.state_)) {}
    OperationLease& operator=(OperationLease&& other) noexcept {
      if (this != &other) {
        Release();
        state_ = std::move(other.state_);
      }
      return *this;
    }

    StorageNode& Native() const { return *state_->native; }

   private:
    void Release() noexcept {
      if (!state_) return;
      {
        std::lock_guard lock(state_->mutex);
        assert(state_->in_flight > 0);
        --state_->in_flight;
      }
      state_->condition.notify_all();
      state_.reset();
    }

    std::shared_ptr<State> state_;
  };

  std::optional<OperationLease> TryAcquire() const {
    auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->phase != Phase::Open) return std::nullopt;
    ++state->in_flight;
    return std::optional<OperationLease>(std::in_place, std::move(state));
  }

  OperationLease Acquire() const {
    return AcquireWithError("StorageNode is stopped");
  }

  OperationLease AcquireDisconnected() const {
    return AcquireWithError("StorageNode is stopped", true);
  }

  OperationLease AcquireWithError(const char* message, bool disconnected = false) const {
    auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->phase != Phase::Open) {
      if (disconnected) throw DisconnectedError(message);
      throw nb::value_error(message);
    }
    ++state->in_flight;
    return OperationLease(std::move(state));
  }

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace sitos::python::detail

void BindStorageNode(nb::module_& python_module) {
  using namespace sitos::python::detail;
  nb::class_<PyInMemoryEngine>(python_module, "InMemoryEngine").def(nb::init<>());
  auto missing = nb::dict();
  python_module.attr("_SESSION_VIEW_MISSING") = missing;
  nb::class_<PySessionView>(python_module, "SessionView")
      .def(
          "get",
          [missing](PySessionView& self, const nb::handle& key, nb::object default_value,
                    nb::object type) { return self.Get(key, default_value, missing, type); },
          "key"_a, nb::kw_only(), "default"_a.none() = missing,
          "type"_a.none() = nb::none())
      .def("contains", &PySessionView::Contains, "key"_a)
      .def("items", &PySessionView::Items, "prefix"_a = "");
  nb::class_<PyStorageNode>(python_module, "StorageNode")
      .def(nb::init<PyInMemoryEngine&, const std::string&, const nb::object&>(), "engine"_a,
           nb::kw_only(), "prefix"_a = "sitos", "zenoh_config_json"_a = nb::none())
      .def("__enter__", &PyStorageNode::Enter, nb::rv_policy::reference_internal)
      .def("__exit__", &PyStorageNode::Exit, "exc_type"_a.none(), "exc_value"_a.none(),
           "traceback"_a.none())
      .def("create_session", &PyStorageNode::CreateSession, "sid"_a)
      .def("close_session", &PyStorageNode::CloseSession, "sid"_a)
      .def("active_sessions", &PyStorageNode::ActiveSessions)
      .def("session_view", &PyStorageNode::SessionViewFor, "sid"_a)
      .def("stop", &PyStorageNode::Stop);
}
