// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Adapter from a Python sitos.StorageEngine to the C++ StorageEngine interface
// (Issue #28, docs/05 §2.4.1).

#ifndef SITOS_PYTHON_BINDINGS_STORAGE_ENGINE_TRAMPOLINE_HPP_
#define SITOS_PYTHON_BINDINGS_STORAGE_ENGINE_TRAMPOLINE_HPP_

#include <nanobind/nanobind.h>

#include <memory>

#include "sitos/storage_engine.hpp"

namespace sitos::python::detail {

/// True when `engine` is an instance of the Python sitos.StorageEngine base class.
bool IsPythonStorageEngine(const nanobind::handle& engine);

/// Wraps a Python sitos.StorageEngine for StorageNode. Every call acquires the GIL;
/// results are copied before the GIL is released and native sinks run (DEC-28-003).
std::shared_ptr<StorageEngine> MakePythonStorageEngine(const nanobind::handle& engine);

/// The engine StorageNode owns for a Python `engine` argument: an InMemoryEngine handle or a
/// Python sitos.StorageEngine. Raises TypeError for anything else. Defined with StorageNode.
std::shared_ptr<StorageEngine> MakeNodeEngine(const nanobind::handle& engine);

void BindStorageEngineTrampoline(nanobind::module_& python_module);

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_STORAGE_ENGINE_TRAMPOLINE_HPP_
