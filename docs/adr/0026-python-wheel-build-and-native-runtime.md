# ADR-0026: Define the Python wheel build and bundled native-runtime boundary

## Status

Proposed — 2026-07-20

## Context

Issue #22 introduces the first Python extension and a distributable wheel. The repository CMake
project is the source of the C++ version, while `python/pyproject.toml` is the scikit-build-core
entry point. Wheel builds must support CPython 3.10–3.13 on Windows and manylinux 2.28 Linux without
requiring a compiler at installation time. The existing prebuilt Linux zenoh-c standalone binary
cannot be relabeled as manylinux 2.28, and the ParamValue-only extension may not retain a linker
edge to the zenoh-c runtime.

## Decision

We will build wheels through the repository root CMake project, derive Python metadata and
`sitos.__version__` from the CMake project version, install only a Python CMake component, and run
non-publishing cibuildwheel validation for the CPython 3.10–3.13 Windows and manylinux 2.28 matrix.
Linux wheel builds will compile pinned zenoh-c sources in the manylinux builder and stage the result
through the explicit `SITOS_ZENOHC_ROOT` CMake cache path; Windows wheels will stage the pinned
official MSVC standalone archive. Repaired wheels explicitly contain the zenoh-c runtime and do not
contain RocksDB or C++ SDK/build artifacts.

## Consequences

* Good: Installed wheels work without Rust, CMake, Ninja, or a C++ compiler.
* Good: The CMake version remains the single source of package version truth.
* Good: Explicit staging makes native dependency provenance and CMake validation visible.
* Bad: Linux wheel builds require a pinned Rust toolchain and are slower than consuming a prebuilt
  standalone archive.
* Bad: Wheel CI must maintain auditwheel/delvewheel repair and native license checks.
* Neutral: Issue #22 builds and tests wheels but does not publish them; publication remains Issue #35.

## Options Considered

* **Relabel the existing Linux standalone binary as manylinux 2.28** — rejected because its GLIBC
  requirements are newer than the wheel policy permits.
* **Use `FETCHCONTENT_SOURCE_DIR_ZENOHC` as the permanent local-source contract** — rejected because
  it hides wheel provisioning behind a FetchContent implementation detail.
* **Bundle only native libraries referenced by the extension** — rejected because codec-only builds
  may omit zenoh-c even though the standard wheel contract includes it.

## References

* Issue #22
* Related: ADR-0006, ADR-0010, ADR-0021
