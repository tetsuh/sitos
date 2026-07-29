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

We will implement RocksDBEngine behind a PImpl-only public header that is installed in both
RocksDB-ON and RocksDB-OFF packages. The OFF build remains linkable: RocksDBEngine::Open returns
Status::Error with std::errc::operation_not_supported. The project configure gate requires
RocksDB 11.1.2 EXACT and the RocksDB::rocksdb target. Installed package discovery is
provider-neutral, relocatable, network-free, and non-bundling; an installed ON package reconstructs
the exact build-time RocksDB dependency, while an installed OFF package discovers no RocksDB
dependency.

We will consume ADR-0004's O(1), copy-free snapshot invariant while making shared DB ownership,
snapshot release, final DB destruction, and filesystem cleanup ordering explicit. Read operations
fully materialize each consistent List read set before invoking user sinks. All sinks are invoked
after native iterator and lock state has been released; sink invocation is never performed under an
engine or native database lock. The reusable contract suite links the production library directly.
Test-only native Open, CRUD, snapshot, and lifecycle seams run in an isolated executable that does
not link the production archive, delegate successful operations to the shared native helpers, and
are not part of the production library or installed archive.

## Consequences

* Good: RocksDB snapshots can outlive the engine while retaining a live DB owner safely.
* Good: `ReleaseSnapshot()` occurs before final DB destruction and directory removal, including on
  Windows where an open LOCK file prevents cleanup.
* Good: The PImpl public header is always installable, and RocksDB-OFF packages remain independent
  of native RocksDB headers and libraries while providing a linkable unsupported Open stub.
* Good: The ON configure and installed-package boundaries require RocksDB 11.1.2 EXACT through
  RocksDB::rocksdb, with provider-neutral, relocatable, network-free, non-bundling discovery and
  exact build-time dependency reconstruction.
* Good: Contract tests execute the production library, while isolated test seams share native
  helpers without introducing duplicate RocksDBEngine definitions or production archive symbols.
* Good: Installed RocksDB-ON consumers validate exact configure and compile/link dependency
  reconstruction without bundling or executing runtime deployment.
* Good: Fully materialized List read sets provide consistent reads and deterministic, unlocked sink
  callbacks that may safely re-enter the engine.
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
