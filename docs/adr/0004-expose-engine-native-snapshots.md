# ADR-0004: Expose engine-native snapshots through the zenoh key space

## Status

Accepted — 2026-07-07

## Context

Compute pipelines must see a consistent point-in-time view of parameters for
the duration of a session, even when external writers are updating values.
The legacy system achieved this with LevelDB snapshots. sitos must preserve
the same semantics while operating over zenoh.

## Decision

We will expose engine-native snapshots through the zenoh key space using
queryables. Each session snapshot is created in O(1) and read in-process
without copying the underlying data.

## Consequences

* Good: Session semantics remain consistent with the legacy system.
* Good: Snapshots are cheap to create and do not block writers.
* Good: External zenoh clients can inspect historical snapshots through the
  queryable interface.
* Bad: Snapshot lifecycle and cleanup must be managed by sitos.

## Options Considered

* **Copy-on-read snapshots** — rejected because it reintroduces copies on the
  hot path and conflicts with the zero-copy goal.
* **Engine-native snapshots exposed via queryables** — selected because it is
  O(1), copy-free, and inspectable from zenoh.

## References

* Issue #1
* Related: ADR-0002, ADR-0003
