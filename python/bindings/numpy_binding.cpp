// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "numpy_binding.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "client_binding.hpp"
#include "numpy_api.hpp"

namespace nb = nanobind;

namespace sitos::python::detail {
namespace {

alignas(std::max_align_t) std::byte kEmptyArraySentinel{};

bool IsSupportedDtype(const PyArray_Descr& descr) {
  if (descr.kind != 'b' && descr.kind != 'i' && descr.kind != 'u' && descr.kind != 'f' &&
      descr.kind != 'c') {
    return false;
  }
  return PyDataType_ELSIZE(&descr) > 0 && !PyDataType_FLAGCHK(&descr, NPY_ITEM_HASOBJECT) &&
         !PyDataType_HASFIELDS(&descr) && !PyDataType_HASSUBARRAY(&descr);
}

[[noreturn]] void ThrowNumpyTypeError(const char* message) {
  PyErr_SetString(PyExc_TypeError, message);
  throw nb::python_error();
}

void ValidateInputArray(const PyArrayObject& array) {
  if (!PyArray_CHKFLAGS(&array, NPY_ARRAY_C_CONTIGUOUS)) {
    throw nb::value_error("NumPy array must be C-contiguous");
  }
  if (!IsSupportedDtype(*PyArray_DESCR(&array))) {
    ThrowNumpyTypeError("NumPy array dtype is unsupported");
  }
}

void DestroyOwner(PyObject* capsule) {
  auto* owner =
      static_cast<std::shared_ptr<const ParamValue>*>(PyCapsule_GetPointer(capsule, nullptr));
  delete owner;
}

nb::object MakeArray(std::span<const std::byte> bytes, std::shared_ptr<const ParamValue> owner,
                     PyArray_Descr* descr) {
  const auto element_size = static_cast<std::size_t>(PyDataType_ELSIZE(descr));
  const auto element_count = bytes.size() / element_size;
  npy_intp dimensions[1] = {static_cast<npy_intp>(element_count)};
  auto owner_holder = std::make_unique<std::shared_ptr<const ParamValue>>(std::move(owner));
  PyObject* capsule = PyCapsule_New(owner_holder.get(), nullptr, DestroyOwner);
  if (capsule == nullptr) {
    Py_DECREF(descr);
    throw nb::python_error();
  }
  owner_holder.release();
  void* data = bytes.empty() ? static_cast<void*>(&kEmptyArraySentinel)
                             : const_cast<std::byte*>(bytes.data());
  PyObject* raw =
      PyArray_NewFromDescr(&PyArray_Type, descr, 1, dimensions, nullptr, data, 0, nullptr);
  if (raw == nullptr) {
    Py_DECREF(capsule);
    throw nb::python_error();
  }
  if (PyArray_SetBaseObject(reinterpret_cast<PyArrayObject*>(raw), capsule) < 0) {
    Py_DECREF(raw);
    throw nb::python_error();
  }
  auto* array = reinterpret_cast<PyArrayObject*>(raw);
  PyArray_CLEARFLAGS(array, NPY_ARRAY_WRITEABLE);
  PyArray_CLEARFLAGS(array, NPY_ARRAY_ALIGNED);
  return nb::steal<nb::object>(nb::handle(raw));
}

}  // namespace

ParamValue ParamValueFromNumpy(const nb::handle& value) {
  if (!PyArray_CheckExact(value.ptr())) {
    throw nb::type_error("expected an exact numpy.ndarray");
  }
  const auto& array = *reinterpret_cast<const PyArrayObject*>(value.ptr());
  ValidateInputArray(array);
  const auto size = static_cast<std::size_t>(PyArray_NBYTES(&array));
  std::vector<std::byte> bytes(size);
  if (size != 0) {
    const auto* source = static_cast<const std::byte*>(PyArray_DATA(&array));
    std::copy(source, source + size, bytes.begin());
  }
  return ParamValue(std::move(bytes));
}

nb::object MakeReadonlyNumpyArray(const SpanHandle<std::byte>& value, const nb::object& dtype) {
  PyArray_Descr* descr = nullptr;
  if (!PyArray_DescrConverter(dtype.ptr(), &descr)) {
    throw nb::python_error();
  }
  if (!IsSupportedDtype(*descr)) {
    Py_DECREF(descr);
    ThrowNumpyTypeError("NumPy dtype is unsupported");
  }
  const auto bytes = std::span<const std::byte>(value.span.data(), value.span.size());
  if (bytes.size() % static_cast<std::size_t>(PyDataType_ELSIZE(descr)) != 0) {
    Py_DECREF(descr);
    ThrowStatus(Status::TypeMismatch, "byte payload size is not divisible by dtype item size");
  }
  return MakeArray(bytes, value.keepalive, descr);
}

}  // namespace sitos::python::detail
