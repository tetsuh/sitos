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
Linux wheel builds will use the pinned
`quay.io/pypa/manylinux_2_28_x86_64@sha256:a61875a2f84cab7df8de222ff12cabc08ff86eb4ad402ac90ba7bdaed9600cca`
builder, whose GCC 14 toolchain supports the core library's C++20 `std::format` use. They compile
pinned zenoh-c sources with the upstream Rust 1.93.0 toolchain and stage the result through the
explicit `SITOS_ZENOHC_ROOT` CMake cache path; Windows wheels will stage the pinned official MSVC
standalone archive.

The Linux build replaces the stale upstream `Cargo.lock` from the zenoh-c 1.9.0 archive with the
repository-owned artifact `third_party/zenoh-c/1.9.0/Cargo.lock` before running
`cargo +1.93.0 build --locked`. The artifact SHA-256 is
`a33695f093ad94cc745d9e5eb9b85a76f5abd63c5c35b66c8c514b0212e1b5a3` and resolves all Zenoh
packages, including root `zenoh-c`, to 1.9.0 from
`release/1.9.0#81c6c933b6e41d72a05f04c4442ef57717ddc72b`. This compensates for upstream release
commit `499de93`, which updated the 1.9.0 manifests but left the archive's lock entries at 1.8.0
and `main#91c230a8...`. The lock was regenerated with Cargo 1.93.0 from the SHA-256-verified
1.9.0 source archive. Updating zenoh-c requires regenerating the artifact with the pinned release
source and toolchain, reviewing the lock diff and dependency provenance, updating its SHA-256,
and updating this ADR before changing the build script.

Linux CI prints the compiler and auditwheel versions, then verifies the repaired wheel with
`auditwheel show` against the `manylinux_2_28_x86_64` policy. Repaired wheels explicitly contain
the zenoh-c runtime and do not contain RocksDB or C++ SDK/build artifacts.

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
