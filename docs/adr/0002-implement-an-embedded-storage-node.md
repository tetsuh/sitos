# ADR-0002: Implement an embedded storage node instead of zenoh storage-manager

## Status

Accepted — 2026-07-07

## Context

zenoh provides a storage-manager plugin that can persist keys. However, sitos
requires fine-grained control over storage engines and, crucially, the ability
to expose engine-native snapshots (for example, RocksDB snapshots) to readers
without copying data. The storage-manager trait does not expose this capability.

## Decision

We will implement our own StorageNode in C++ instead of relying on the zenoh
storage-manager plugin.

## Consequences

* Good: Storage engines can expose native snapshots directly to queryables.
* Good: New storage engines can be added without writing Rust plugins.
* Good: Snapshot semantics are under sitos control and remain O(1).
* Bad: We are responsible for persistence, replication, and recovery logic
  that the storage-manager would otherwise provide.

## Options Considered

* **zenoh storage-manager plugin** — rejected because the storage trait does
  not expose engine snapshots, and adding new engines would require Rust plugin
  development.
* **Embedded StorageNode in C++** — selected because it keeps engine abstraction
  and snapshot semantics entirely within the sitos codebase.

## References

* Issue #1
* Related: ADR-0001, ADR-0004
