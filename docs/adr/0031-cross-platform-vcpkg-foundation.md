# ADR-0031: Establish a cross-platform vcpkg foundation

## Status

Proposed — pending owner acceptance immediately before merge

## Context

sitos needs a reproducible provider for optional native dependencies on Linux and Windows without
changing the default RocksDB-OFF build or adding package-manager behavior to project CMake. The
reviewed vcpkg commit resolves RocksDB 11.1.2, port-version 0, with zlib as its only default
feature. CI must remain correct when its optional binary cache is unavailable.

## Decision

We will use an explicit root `vcpkg.json` with no ordinary dependencies and a non-default `rocksdb`
feature, and use its `builtin-baseline` as the sole executable and registry pin. Dedicated CI jobs
will check out official vcpkg at that baseline and use built-in `x64-linux` and `x64-windows`
triplets. A SHA-pinned GitHub-owned `actions/cache` action persists a dedicated filesystem binary
archive supplied to vcpkg through `clear;files,<directory>,readwrite`. Primary keys include cache
format, OS, architecture, triplet, manifest hash, and GitHub-hosted runner image identity. A
compatible-prefix restore may seed a new runner-image key, but only an exact primary-key hit skips
save. The cache is an optional performance optimization, not a correctness dependency.

## Consequences

* Good: Optional native provisioning is auditable and reproducible across the supported CI platforms.
* Good: Explicit isolated install roots and a consumer probe catch target, link, and runtime errors.
* Good: Pull-request cache writes are constrained to the pull request merge-ref scope, and no
  external cache credential, Mono runtime, NuGet client, or GitHub Packages feed is required.
* Good: vcpkg remains responsible for package-ABI validation inside the cached archive directory.
* Bad: GitHub cache entries are immutable and subject to repository quotas and eviction; the
  runner-image key component or cache-format version changes when the outer compatibility boundary
  changes.
* Bad: Cold provisioning compiles RocksDB and can take substantial time; the initial budgets are 180
  minutes for Linux and 360 minutes for Windows.
* Bad: Homebrew/macOS provisioning is unsupported, and `apt` is limited to Linux CI prerequisites.
* Neutral: Cache misses, eviction, or unavailable credentials do not alter correctness.
* Windows runtime: the canonical static `RocksDB::rocksdb` target imports the pinned zlib 1.3.2
  runtime as `z.dll`; validation stages that actual file, requires `dumpbin` from the `vcvars64.bat`
  environment, and rejects `zlib1.dll` compatibility copies and `rocksdb-shared.dll` dependencies.

## Options Considered

* **Runner-provided vcpkg** — rejected because a mutable checkout is not reproducible.
* **A custom triplet or static Windows triplet** — rejected because this foundation validates the
  canonical built-in triplets and their actual runtime behavior.
* **Automatic provisioning from project or installed CMake** — rejected because package discovery
  must remain network-free and under the caller's explicit provisioning boundary.
* **Removed vcpkg `x-gha` backend** — rejected because the pinned vcpkg executable reports that the
  backend has been removed.
* **GitHub Packages through vcpkg's NuGet backend** — rejected because it adds Mono and `nuget.exe`
  on Linux, feed authentication, and package permissions that are unnecessary for this repository.
* **Caching the installed dependency tree** — rejected because caching vcpkg's package-ABI archives
  retains vcpkg's own reuse and validation boundary.
* **External or long-lived cache credentials** — rejected because GitHub's merge-ref-scoped cache
  action needs no repository secret or external credential.

## References

* Issue #122
* ADR-0021: Resolve installed Zenoh dependencies without fetching or bundling
* `docs/06_build_test_packaging.md`
* `docs/09_dependency_policy.md`
