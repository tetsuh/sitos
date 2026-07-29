# ADR-0033: Define the RocksDB engine, snapshot, and installed-package boundary

## Status

Proposed — 2026-07-28

## Context

Issue #8 adds an optional persistent RocksDB implementation of `StorageEngine` and gives the
installed CMake package an external dependency on an exact RocksDB version. ADR-0004 governs O(1),
copy-free native snapshot semantics but does not define ownership or filesystem-cleanup ordering.
The implementation must preserve the existing `StorageEngine` contract, remain available in
RocksDB-OFF builds, avoid implicit package-manager activity, and keep runtime deployment outside
installed-package validation.

## Decision

We will install a PImpl-only `RocksDBEngine` public header in both RocksDB-ON and RocksDB-OFF
packages, keep the OFF build linkable by making `RocksDBEngine::Open` return `Status::Error` with
`std::errc::operation_not_supported`, require RocksDB 11.1.2 EXACT and `RocksDB::rocksdb` at
configuration time, and make installed-package dependency reconstruction exact, provider-neutral,
relocatable, network-free, and non-bundling; an installed RocksDB-OFF package will neither discover
nor require RocksDB. We will preserve ADR-0004's O(1), copy-free snapshot invariant by retaining
shared database ownership until snapshot release; each `List` operation will fully materialize a
consistent read set before invoking user sinks, and no sink will run while an engine lock, native
database lock, or iterator is held. We will run the reusable contract suite against the production
library and build test-only native Open, CRUD, snapshot, and lifecycle seams into a separate
executable that does not link the production archive; successful CRUD and snapshot seam operations
will delegate to shared native helpers, and no seam symbols will enter the production library or
installed archive.

## Consequences

* Good: RocksDB snapshots can outlive the engine because they retain shared ownership of the live
  database state.
* Good: `ReleaseSnapshot()` occurs before final database destruction and directory removal,
  including on Windows, where an open `LOCK` file prevents cleanup.
* Good: The PImpl public header is always installable, and RocksDB-OFF packages remain independent
  of native RocksDB headers and libraries while providing a linkable `Open` stub that reports an
  unsupported operation.
* Good: RocksDB-ON configuration and installed-package reconstruction require RocksDB 11.1.2 EXACT
  through `RocksDB::rocksdb`, with provider-neutral, relocatable, network-free, and non-bundling
  discovery.
* Good: Contract tests execute the production library, while isolated test seams reuse native
  helpers without introducing duplicate `RocksDBEngine` definitions or production archive symbols.
* Good: Installed RocksDB-ON consumers validate exact dependency reconstruction during
  configuration and compile/link validation without bundling dependencies or executing the
  installed consumer.
* Good: Fully materialized `List` read sets provide consistent reads and deterministic, unlocked
  sink callbacks that may safely re-enter the engine.
* Bad: Exact version equality is necessary but not sufficient for ABI compatibility. External
  consumers remain responsible for compiler, CRT, standard-library ABI, build configuration, and
  relevant compile options.
* Neutral: Executed engine tests and the existing vcpkg clean-stage probe validate RocksDB runtime
  behavior, while installed consumers remain build/link-only.

## Options Considered

* **Copy snapshots into a separate map** — rejected because ADR-0004 requires O(1), copy-free native
  snapshots for RocksDB.
* **Destroy the database with the engine regardless of live snapshots** — rejected because native
  snapshot handles require the database to remain alive until `ReleaseSnapshot()`.
* **Remove the database directory in the engine destructor** — rejected because paths are
  user-owned, and the Windows `LOCK`-file lifetime makes cleanup an explicit owner responsibility.
* **Allow any compatible RocksDB version in installed packages** — rejected until cross-version
  compile, link, and runtime evidence exists; the exact build-time version is the current boundary.
* **Execute installed RocksDB consumers** — rejected because runtime deployment belongs to the
  application or package manager under ADR-0021; the Issue #122 clean-stage probe provides runtime
  evidence.

## References

* Issue #8 / PR #128
* ADR-0004: Expose engine-native snapshots through the zenoh key space
* ADR-0021: Resolve installed Zenoh dependencies without fetching or bundling
* ADR-0031: Establish a cross-platform vcpkg foundation
