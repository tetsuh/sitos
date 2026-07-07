# ADR-0006: C++20 core with Python bindings

## Status

Accepted — 2026-07-07

## Context

sitos needs a high-performance core with zero-copy access to parameter values,
plus an easy-to-install Python interface for compute pipelines. The design
must avoid heavy legacy dependencies such as Boost and should leverage modern
standard-library features.

## Decision

We will implement the core in C++20 and provide Python bindings via nanobind.

## Consequences

* Good: C++20 provides `std::span` for zero-copy views without a Boost dependency.
* Good: nanobind is lightweight and offers good buffer-protocol and GIL control.
* Good: A single `pip install` can deliver both C++ and Python users.
* Neutral: Build tooling must support both C++ and Python wheel packaging.

## Options Considered

* **C++17 + pybind11** — rejected because it misses `std::span` and pybind11
  is heavier than nanobind.
* **C++20 + Boost** — rejected to avoid the Boost dependency.
* **C++20 + nanobind** — selected for modern standard-library features and a
  lightweight binding layer.

## References

* Issue #1
* Related: ADR-0010
