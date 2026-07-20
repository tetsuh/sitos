# ADR-0026: Define the Python wheel build and bundled native-runtime boundary

## Status

Proposed — 2026-07-20

## Context

Issue #22 introduces the first Python extension and a distributable wheel. The repository CMake
project is the source of the C++ version, while `python/pyproject.toml` is the scikit-build-core
entry point. The production wheel target is CPython 3.12 `manylinux_2_28_x86_64`, validated on
Ubuntu 24.04 and Rocky Linux 10. CPython 3.12 `win_amd64` is non-publishing build/test coverage
only. Other Python versions, Ubuntu 26.04, and formal Windows publication are deferred.

The existing prebuilt Linux zenoh-c standalone binary cannot be relabeled as manylinux 2.28. The
standard wheel must contain one, and only one, zenoh-c runtime without requiring a compiler at
installation time. The ParamValue-only extension may not retain a linker edge without explicitly
linking the Zenoh-enabled `sitos` target.

## Decision

We build wheels through the repository root CMake project, derive Python metadata and
`sitos.__version__` from the CMake project version, install only a Python CMake component, and
perform non-publishing cibuildwheel validation for CPython 3.12 on Linux and Windows. Linux uses
the pinned `quay.io/pypa/manylinux_2_28_x86_64@sha256:a61875a2f84cab7df8de222ff12cabc08ff86eb4ad402ac90ba7bdaed9600cca`
builder. Windows stages the pinned official MSVC standalone archive through the explicit
`SITOS_ZENOHC_ROOT` CMake cache path.

The Python CMake component installs only the extension and Python package sources. It never copies
the zenoh-c runtime. `auditwheel repair` (Linux) and `delvewheel repair` (Windows) exclusively own
runtime bundling. Post-repair validation requires exactly one bundled zenoh-c library, verifies the
extension declares and resolves its native dependency against that wheel-local runtime, and performs
an exact-filename `pip install --only-binary=:all:` clean-environment import/conversion check with
staging and loader paths removed. The standard wheel links `sitos` to `zenohc::zenohc` deliberately;
codec-only linkage is not the selected contract.

Both zenoh-c `LICENSE` and `NOTICE.md` are copied from the SHA-256-verified 1.9.0 upstream source
into unambiguous wheel distribution license metadata names. Wheel validation rejects RocksDB,
GTest/GMock, CMake exports, headers, static libraries, benchmarks, and build artifacts.

Production Linux wheels install checksum-verified `rustup-init`, use Rust 1.93.0, replace the stale
zenoh-c 1.9.0 archive lock with repository-owned
`third_party/zenoh-c/1.9.0/Cargo.lock`, and run `cargo --locked`. Its SHA-256 is
`a33695f093ad94cc745d9e5eb9b85a76f5abd63c5c35b66c8c514b0212e1b5a3`; it resolves Zenoh 1.9.0
through `release/1.9.0#81c6c933b6e41d72a05f04c4442ef57717ddc72b`. Updating zenoh-c requires
regenerating and reviewing this lock from the verified release source, updating its hash, and
updating this ADR. A scheduled, default-branch-dispatched, or explicitly `run-latest-compatible`-labeled PR,
non-publishing compatibility lane uses moving Rust stable with the same fixed lock and builds a
wheel after selecting the latest compatible `build`, `cibuildwheel`, `scikit-build-core`, `nanobind`,
and `delvewheel`. It never selects the production runtime, uploads an artifact, or publishes one.

Production workflow Python tooling is installed from `.github/wheel-tools-requirements.txt` using
exact versions, binary-only downloads, and reviewed SHA-256 hashes for direct and transitive
packages. `PIP_CONSTRAINT`, `PIP_REQUIRE_HASHES`, and `PIP_ONLY_BINARY` are passed into the actual
cibuildwheel build-isolation environment, so the lock governs backend and repair tooling rather
than only the host launcher. Post-repair validation creates a separate clean venv, hash-installs
its fetched test and runtime dependencies from the same lock, then installs the newly generated
local wheel with `--no-deps`. That generated wheel cannot have a pre-build hash; CI records its
SHA-256 before installation, and `--no-deps` ensures it is the only install outside hash enforcement.
The current non-yanked Windows repair tool is delvewheel 1.13.0. `auditwheel` is supplied by the
pinned cibuildwheel manylinux image rather than a host pip installation; CI prints its actual
version. Updating the builder image or wheel-tool lock requires reviewing dependency/tool
provenance and rerunning both wheel jobs. Rocky Linux 10 clean-install validation uses
`rockylinux/rockylinux@sha256:e372170ca8630f0f03e9b70fdd0bf4a3ce3426b0de7cdba615f06337389de176`.

## Consequences

* Good: Installed production wheels work without Rust, CMake, Ninja, or a C++ compiler.
* Good: The CMake version remains the single source of package-version truth.
* Good: Runtime ownership, dependency resolution, and license provenance are unambiguous.
* Good: The pinned production graph and latest-Rust compatibility signal are distinct.
* Bad: Linux wheel builds require a pinned Rust toolchain and are slower than consuming a prebuilt
  standalone archive.
* Bad: Wheel CI must maintain repair, clean-install, native-dependency, and third-party license checks.
* Neutral: Issue #22 validates wheels but does not publish them; publication remains Issue #35.

## Options Considered

* **Relabel the existing Linux standalone binary as manylinux 2.28** — rejected because its GLIBC
  requirements are newer than the wheel policy permits.
* **Install zenoh-c directly from the Python CMake component** — rejected because it duplicates the
  repair tool's runtime and allows a wheel to carry conflicting native libraries.
* **Use `FETCHCONTENT_SOURCE_DIR_ZENOHC` as the permanent local-source contract** — rejected because
  it hides wheel provisioning behind a FetchContent implementation detail.
* **Bundle only native libraries referenced by the extension** — rejected because codec-only builds
  may omit zenoh-c even though the standard wheel contract includes it.

## References

* Issue #22
* Related: ADR-0006, ADR-0010, ADR-0021
