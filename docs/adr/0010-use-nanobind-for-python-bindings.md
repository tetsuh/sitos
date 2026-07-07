# ADR-0010: Use nanobind for Python bindings

## Status

Accepted — 2026-07-07

## Context

The Python bindings must expose C++ types with minimal overhead, support the
buffer protocol for NumPy zero-copy views, and allow precise control over the
GIL to avoid deadlocks when Python callbacks invoke sitos methods.

## Decision

We will use nanobind for the Python bindings.

## Consequences

* Good: nanobind is smaller and faster than pybind11.
* Good: It supports buffer protocol and explicit GIL release as first-class
  features.
* Good: It integrates cleanly with scikit-build-core for wheel builds.
* Neutral: The team must learn nanobind-specific idioms.

## Options Considered

* **pybind11** — rejected because it is larger and slower, and GIL control is
  less explicit.
* **Cython** — rejected because it would require maintaining a separate Cython
  layer and does not reuse the C++ header definitions as directly.
* **nanobind** — selected for performance, size, and GIL/buffer control.

## References

* Issue #1
* Related: ADR-0006
