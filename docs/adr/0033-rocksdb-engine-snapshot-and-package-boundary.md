# ADR-0033: Define the RocksDB engine, snapshot, and installed-package boundary

## Status

Proposed

## Context

Issue #8 adds the optional persistent RocksDB StorageEngine and extends the installed CMake package
with an exact external RocksDB dependency. ADR-0004 is the sole authority for O(1), copy-free native
snapshot semantics, but it leaves ownership and filesystem cleanup ordering open. The implementation
must remain available in RocksDB-OFF builds, preserve the existing StorageEngine contract, and avoid
hidden package-manager or runtime deployment behavior.

## Decision

We will implement RocksDBEngine behind a PImpl-only public header, require the exact build-time
RocksDB version when reconstructing RocksDB-ON installed packages, and keep RocksDB-OFF packages free
of the dependency. We will consume ADR-0004's O(1), copy-free snapshot invariant while making shared
DB ownership, snapshot release, final DB destruction, and filesystem cleanup ordering explicit.

## Consequences

* Good: RocksDB snapshots can outlive the engine while retaining a live DB owner safely.
* Good: `ReleaseSnapshot()` occurs before final DB destruction and directory removal, including on
  Windows where an open LOCK file prevents cleanup.
* Good: Public headers and RocksDB-OFF packages remain independent of native RocksDB headers and
  libraries.
* Good: Installed RocksDB-ON consumers validate exact configure and compile/link dependency
  reconstruction without bundling or executing runtime deployment.
* Bad: Exact version equality is necessary but not sufficient for ABI compatibility. External
  consumers remain responsible for compiler, CRT, standard-library ABI, build configuration, and
  relevant compile options.
* Neutral: Runtime RocksDB deployment is validated by the executed engine tests and the existing
  vcpkg clean-stage probe, while installed consumers are build/link-only.

## Options Considered

* **Copy snapshots into a separate map** — rejected for RocksDB because ADR-0004 requires O(1),
  copy-free native snapshots.
* **Destroy the DB with the engine regardless of snapshots** — rejected because native snapshot
  handles require the DB to remain alive until `ReleaseSnapshot()`.
* **Remove the database directory from the engine destructor** — rejected because user-owned paths
  and Windows LOCK-file lifetime make cleanup an explicit owner responsibility.
* **Allow any compatible RocksDB version in installed packages** — rejected until cross-version
  compiler/link/run evidence exists; the build-time exact version is the current boundary.
* **Execute installed RocksDB consumers** — rejected because runtime deployment belongs to the
  application or package manager under ADR-0021; the #122 clean-stage probe owns runtime evidence.

## References

* Issue #8 / PR implementing Issue #8
* ADR-0004: Expose engine-native snapshots through the zenoh key space
* ADR-0021: Resolve installed Zenoh dependencies without fetching or bundling
* ADR-0031: Establish a cross-platform vcpkg foundation
