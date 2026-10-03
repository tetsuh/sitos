// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Adapter from a Python sitos.StorageEngine to the C++ StorageEngine interface
// (Issue #28, docs/05 §2.4.1).

#include "storage_engine_trampoline.hpp"

#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {

namespace {

using Entries = std::vector<std::pair<std::string, std::vector<std::byte>>>;

/// Thrown into StorageNode for a failed Python read; StorageNode already contains
/// engine exceptions (DEC-28-002).
class EngineFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

bool PythonUsable() noexcept {
#if PY_VERSION_HEX >= 0x030D0000
  return Py_IsInitialized() != 0 && Py_IsFinalizing() == 0;
#else
  return Py_IsInitialized() != 0 && _Py_IsFinalizing() == 0;
#endif
}

nb::object SitosLogger() {
  return nb::module_::import_("logging").attr("getLogger")("sitos");
}

// Requires the GIL. Logging must never turn an engine failure into a different error.
void LogPythonError(const char* operation, nb::python_error& error) noexcept {
  try {
    SitosLogger().attr("error")(
        "sitos Python storage engine %s raised; treated as an engine failure", operation,
        "exc_info"_a = nb::make_tuple(error.type(), error.value(), error.traceback()));
  } catch (...) {
    // Logging must not turn the engine failure into a different error.
  }
}

void LogEngineMessage(const char* operation, const char* problem) noexcept {
  try {
    SitosLogger().attr("error")("sitos Python storage engine %s %s; treated as an engine failure",
                                operation, problem);
  } catch (...) {
    // Logging must not turn the engine failure into a different error.
  }
}

/// Owns one strong reference; the last owner may run on a native thread without the GIL,
/// so the decref takes the GIL and is skipped once the interpreter is finalizing.
class PythonReference {
 public:
  explicit PythonReference(const nb::handle& object) : object_(object.inc_ref().ptr()) {}
  PythonReference(const PythonReference&) = delete;
  PythonReference& operator=(const PythonReference&) = delete;
  ~PythonReference() {
    if (!PythonUsable()) return;  // leak rather than touch a finalizing interpreter
    nb::gil_scoped_acquire gil;
    Py_DECREF(object_);
  }

  nb::handle get() const { return object_; }

 private:
  PyObject* object_;
};

/// Runs `call` holding the GIL, or throws when the interpreter can no longer run Python. The
/// check is a best-effort backstop: the exit handler stops every Python-engine node, waiting for
/// in-flight calls, before finalization (DEC-28-007).
template <typename Call>
auto WithGil(Call&& call) {
  if (!PythonUsable()) throw EngineFailure("Python interpreter is finalizing");
  nb::gil_scoped_acquire gil;
  return std::forward<Call>(call)();
}

std::vector<std::byte> CopyBytes(PyObject* value) {
  const auto* data = reinterpret_cast<const std::byte*>(PyBytes_AS_STRING(value));
  return {data, data + PyBytes_GET_SIZE(value)};
}

std::optional<std::vector<std::byte>> CallGet(const nb::handle& reader, std::string_view key) {
  return WithGil([&]() -> std::optional<std::vector<std::byte>> {
    nb::object result;
    try {
      result = reader.attr("get")(nb::str(key.data(), key.size()));
    } catch (nb::python_error& error) {
      LogPythonError("get", error);
      throw EngineFailure("Python storage engine get raised");
    }
    if (result.is_none()) return std::nullopt;
    if (!PyBytes_CheckExact(result.ptr())) {
      LogEngineMessage("get", "returned a value that is not bytes or None");
      throw EngineFailure("Python storage engine get returned an invalid value");
    }
    return CopyBytes(result.ptr());
  });
}

// Requires the GIL. Materializes the complete result before any sink runs, then orders it by
// key bytes, which is UTF-8 code point order (DEC-28-003; C++ List contract).
Entries ListWithGil(const nb::handle& reader, std::string_view prefix) {
  Entries entries;
  try {
    nb::object result = reader.attr("list")(nb::str(prefix.data(), prefix.size()));
    for (nb::handle item : result) {
      PyObject* key = nullptr;
      PyObject* value = nullptr;
      if (PyTuple_CheckExact(item.ptr()) && PyTuple_GET_SIZE(item.ptr()) == 2) {
        key = PyTuple_GET_ITEM(item.ptr(), 0);
        value = PyTuple_GET_ITEM(item.ptr(), 1);
      }
      if (key == nullptr || !PyUnicode_Check(key) || !PyBytes_CheckExact(value)) {
        LogEngineMessage("list", "returned an entry that is not a (str, bytes) pair");
        throw EngineFailure("Python storage engine list returned an invalid entry");
      }
      Py_ssize_t size = 0;
      const char* utf8 = PyUnicode_AsUTF8AndSize(key, &size);
      if (utf8 == nullptr) throw nb::python_error();
      entries.emplace_back(std::string(utf8, static_cast<std::size_t>(size)), CopyBytes(value));
    }
  } catch (nb::python_error& error) {
    LogPythonError("list", error);
    throw EngineFailure("Python storage engine list raised");
  }
  std::ranges::sort(entries, {}, &Entries::value_type::first);
  return entries;
}

Entries CallList(const nb::handle& reader, std::string_view prefix) {
  return WithGil([&] { return ListWithGil(reader, prefix); });
}

bool DeliverList(const Entries& entries, const EntrySink& sink) {
  return std::ranges::all_of(entries, [&sink](const auto& entry) {
    return sink(entry.first, entry.second);
  });
}

class PythonStorageReader final : public StorageReader {
 public:
  explicit PythonStorageReader(const nb::handle& reader) : reader_(reader) {}

  bool Get(std::string_view key, const EntrySink& sink) const override {
    auto value = CallGet(reader_.get(), key);
    if (!value) return false;
    sink(key, *value);
    return true;
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    return DeliverList(CallList(reader_.get(), prefix), sink);
  }

 private:
  PythonReference reader_;
};

class PythonStorageEngine final : public StorageEngine {
 public:
  explicit PythonStorageEngine(const nb::handle& engine) : engine_(engine) {}

  bool Get(std::string_view key, const EntrySink& sink) const override {
    auto value = CallGet(engine_.get(), key);
    if (!value) return false;
    sink(key, *value);
    return true;
  }

  bool List(std::string_view prefix, const EntrySink& sink) const override {
    return DeliverList(CallList(engine_.get(), prefix), sink);
  }

  bool Put(std::string_view key, Bytes value) override {
    return CallWrite("put", [&] {
      return engine_.get().attr("put")(
          nb::str(key.data(), key.size()),
          nb::bytes(reinterpret_cast<const char*>(value.data()), value.size()));
    });
  }

  bool Delete(std::string_view key) override {
    return CallWrite("delete",
                     [&] { return engine_.get().attr("delete")(nb::str(key.data(), key.size())); });
  }

  // DEC-28-004: an engine without take_snapshot uses the C++ copy fallback (N03).
  std::shared_ptr<const StorageReader> TakeSnapshot() const override {
    auto reader = WithGil([this] { return PythonSnapshotWithGil(); });
    return reader ? reader : StorageEngine::TakeSnapshot();
  }

 private:
  // DEC-28-002: only an explicit False or an exception reports a failed write.
  template <typename Call>
  bool CallWrite(const char* operation, Call&& call) {
    if (!PythonUsable()) return false;
    nb::gil_scoped_acquire gil;
    try {
      nb::object result = std::forward<Call>(call)();
      return result.ptr() != Py_False;
    } catch (nb::python_error& error) {
      LogPythonError(operation, error);
      return false;
    }
  }

  // Requires the GIL. Returns null when the engine has no take_snapshot.
  std::shared_ptr<const StorageReader> PythonSnapshotWithGil() const {
    if (!nb::hasattr(engine_.get(), "take_snapshot")) return nullptr;
    try {
      nb::object reader = engine_.get().attr("take_snapshot")();
      if (nb::isinstance(reader, nb::module_::import_("sitos.engine").attr("StorageReader"))) {
        return std::make_shared<PythonStorageReader>(reader);
      }
    } catch (nb::python_error& error) {
      LogPythonError("take_snapshot", error);
      throw EngineFailure("Python storage engine take_snapshot raised");
    }
    LogEngineMessage("take_snapshot", "returned an object that is not a StorageReader");
    throw EngineFailure("Python storage engine take_snapshot returned an invalid reader");
  }

  PythonReference engine_;
};

#if SITOS_PYTHON_TEST_SUPPORT
// Source-only probe: drives the exact StorageEngine object StorageNode would own, from
// native code with the GIL released, as a zenoh thread would.
nb::object EntriesToPython(const Entries& entries) {
  nb::list rows;
  for (const auto& [key, value] : entries) {
    rows.append(nb::make_tuple(
        nb::str(key.data(), key.size()),
        nb::bytes(reinterpret_cast<const char*>(value.data()), value.size())));
  }
  return rows;
}

class ProbeSink {
 public:
  ProbeSink(nb::object action, bool result, std::optional<std::size_t> stop_after)
      : action_(std::move(action)), result_(result), stop_after_(stop_after) {}

  EntrySink Sink() {
    return [this](std::string_view key, Bytes value) {
      entries_.emplace_back(std::string(key), std::vector<std::byte>(value.begin(), value.end()));
      if (!action_.is_none()) {
        nb::gil_scoped_acquire gil;
        try {
          action_();
        } catch (nb::python_error& error) {
          if (!error_) error_ = error.what();
        }
      }
      if (stop_after_.has_value() && entries_.size() >= *stop_after_) return false;
      return result_;
    };
  }

  nb::object Finish(bool found) {
    if (error_) throw std::runtime_error("probe action failed: " + *error_);
    return nb::make_tuple(found, EntriesToPython(entries_));
  }

 private:
  nb::object action_;
  bool result_;
  std::optional<std::size_t> stop_after_;
  Entries entries_;
  std::optional<std::string> error_;
};

template <typename Reader>
nb::object ProbeGet(const Reader& reader, const std::string& key, nb::object action,
                    bool sink_result) {
  ProbeSink sink(std::move(action), sink_result, std::nullopt);
  bool found = false;
  try {
    nb::gil_scoped_release release;
    found = reader.Get(key, sink.Sink());
  } catch (const EngineFailure& failure) {
    throw std::runtime_error(std::string("engine failure: ") + failure.what());
  }
  return sink.Finish(found);
}

template <typename Reader>
nb::object ProbeList(const Reader& reader, const std::string& prefix, nb::object action,
                     std::optional<std::size_t> stop_after) {
  ProbeSink sink(std::move(action), true, stop_after);
  bool completed = false;
  try {
    nb::gil_scoped_release release;
    completed = reader.List(prefix, sink.Sink());
  } catch (const EngineFailure& failure) {
    throw std::runtime_error(std::string("engine failure: ") + failure.what());
  }
  return sink.Finish(completed);
}

class EngineReaderProbe {
 public:
  explicit EngineReaderProbe(std::shared_ptr<const StorageReader> reader)
      : reader_(std::move(reader)) {}
  ~EngineReaderProbe() {
    nb::gil_scoped_release release;
    reader_.reset();
  }

  nb::object Get(const std::string& key, nb::object action, bool sink_result) {
    return ProbeGet(*reader_, key, std::move(action), sink_result);
  }
  nb::object List(const std::string& prefix, nb::object action,
                  std::optional<std::size_t> stop_after) {
    return ProbeList(*reader_, prefix, std::move(action), stop_after);
  }

 private:
  std::shared_ptr<const StorageReader> reader_;
};

class EngineProbe {
 public:
  explicit EngineProbe(nb::object engine)
      : engine_object_(std::move(engine)), engine_(MakeNodeEngine(engine_object_)) {}
  ~EngineProbe() { DropWithoutGil(); }

  bool Put(const std::string& key, const nb::bytes& value) {
    const auto* data = reinterpret_cast<const std::byte*>(value.c_str());
    std::vector<std::byte> copy(data, data + value.size());
    nb::gil_scoped_release release;
    return engine_->Put(key, copy);
  }
  bool Delete(const std::string& key) {
    nb::gil_scoped_release release;
    return engine_->Delete(key);
  }
  nb::object Get(const std::string& key, nb::object action, bool sink_result) {
    return ProbeGet(*engine_, key, std::move(action), sink_result);
  }
  nb::object List(const std::string& prefix, nb::object action,
                  std::optional<std::size_t> stop_after) {
    return ProbeList(*engine_, prefix, std::move(action), stop_after);
  }
  EngineReaderProbe Snapshot() {
    try {
      nb::gil_scoped_release release;
      return EngineReaderProbe(engine_->TakeSnapshot());
    } catch (const EngineFailure& failure) {
      throw std::runtime_error(std::string("engine failure: ") + failure.what());
    }
  }
  int SyncCapability() const { return static_cast<int>(engine_->GetSyncCapability()); }
  bool Sync() {
    nb::gil_scoped_release release;
    return engine_->Sync().IsOk();
  }
  nb::object Engine() const { return engine_object_; }

  // Drops the native owner on this thread with the GIL released, as a node thread would.
  void DropWithoutGil() {
    engine_object_ = nb::none();
    nb::gil_scoped_release release;
    engine_.reset();
  }

 private:
  nb::object engine_object_;
  std::shared_ptr<StorageEngine> engine_;
};
#endif  // SITOS_PYTHON_TEST_SUPPORT

}  // namespace

bool IsPythonStorageEngine(const nb::handle& engine) {
  nb::object engine_type = nb::module_::import_("sitos.engine").attr("StorageEngine");
  return nb::isinstance(engine, engine_type);
}

std::shared_ptr<StorageEngine> MakePythonStorageEngine(const nb::handle& engine) {
  return std::make_shared<PythonStorageEngine>(engine);
}

void BindStorageEngineTrampoline([[maybe_unused]] nb::module_& python_module) {
#if SITOS_PYTHON_TEST_SUPPORT
  nb::class_<EngineReaderProbe>(python_module, "_EngineReaderProbe")
      .def("get", &EngineReaderProbe::Get, "key"_a, "action"_a.none() = nb::none(),
           "sink_result"_a = true)
      .def("list", &EngineReaderProbe::List, "prefix"_a, "action"_a.none() = nb::none(),
           "stop_after"_a.none() = nb::none());
  nb::class_<EngineProbe>(python_module, "_EngineProbe")
      .def(nb::init<nb::object>(), "engine"_a)
      .def("put", &EngineProbe::Put, "key"_a, "value"_a)
      .def("delete", &EngineProbe::Delete, "key"_a)
      .def("get", &EngineProbe::Get, "key"_a, "action"_a.none() = nb::none(),
           "sink_result"_a = true)
      .def("list", &EngineProbe::List, "prefix"_a, "action"_a.none() = nb::none(),
           "stop_after"_a.none() = nb::none())
      .def("snapshot", &EngineProbe::Snapshot)
      .def("sync_capability", &EngineProbe::SyncCapability)
      .def("sync", &EngineProbe::Sync)
      .def("drop_without_gil", &EngineProbe::DropWithoutGil)
      .def_prop_ro("engine", &EngineProbe::Engine);
#endif
}

}  // namespace sitos::python::detail
