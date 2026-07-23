// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PYTHON_BINDINGS_NUMPY_API_HPP_
#define SITOS_PYTHON_BINDINGS_NUMPY_API_HPP_

#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_2_0_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL SITOS_NUMPY_API
#if !defined(SITOS_NUMPY_IMPORT)
#define NO_IMPORT_ARRAY
#endif
#include <numpy/arrayobject.h>

#endif  // SITOS_PYTHON_BINDINGS_NUMPY_API_HPP_
