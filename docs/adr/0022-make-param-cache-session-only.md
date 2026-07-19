# ADR-0022: Make ParamCache session-only

## Status

Accepted — 2026-07-19

## Context

`ParamCache::AttachBase()` exposes a standalone live-base cache mode in addition to the
session snapshot-plus-overlay cache used by compute-side participants. The mode is not
required by the current MUST requirements and gives ParamCache writes no session overlay
destination. It also forces lifecycle, callback, and future read/write APIs to carry an
unnecessary base/session distinction.

## Decision

We will remove `ParamCache::AttachBase()` and require ParamCache to attach only with an
explicit syntactically valid session id. ParamCache will not perform a session-existence
preflight; an unknown or empty session may attach successfully as an empty cache, as defined
by Issue #18. Base access remains available through ParamStore scope `"base"` and host-side
StorageNode/StorageEngine APIs.

## Consequences

* Good: Every attached ParamCache has an unambiguous session overlay and snapshot baseline.
* Good: ParamCache lifecycle, defensive sample validation, and future read/write APIs no longer
  need a base-mode branch.
* Good: Existing StorageNode and ParamStore base routing is unchanged.
* Bad: Existing consumers of the pre-1.0 `AttachBase()` API must migrate to ParamStore or an
  explicit session.
* Neutral: A dedicated base hot-path cache is not provided; it can be proposed separately if a
  concrete requirement emerges.

## Options Considered

* **Keep AttachBase alongside Attach** — rejected because it preserves an unnecessary dual mode
  and leaves session-only writes ambiguous for base-attached caches.
* **Make AttachBase writable against base** — rejected because ParamCache writes are session-only
  and base writes belong to ParamStore/StorageNode APIs.
* **Perform session metadata preflight in Attach** — rejected because it changes Issue #18's
  zero-reply semantics and introduces a separate existence/TOCTOU contract.

## References

* Issue #18 — ParamCache initial fetch and delta subscription
* Issue #19 — ParamCache reads, writes, and performance
* Issue #100 — Remove ParamCache AttachBase mode
* [ADR process](../10_adr_process.md)
