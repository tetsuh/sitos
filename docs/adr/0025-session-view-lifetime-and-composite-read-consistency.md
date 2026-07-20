# ADR-0025: Define SessionView lifetime and composite-read consistency

## Status

Accepted — 2026-07-20

## Context

SessionView is an in-process facade that composes one active session overlay over its immutable
snapshot. StorageNode readers expose callback-scoped byte views, while node Stop and session Close
may run concurrently with view operations. The public API therefore needs explicit ownership,
lifetime, and callback consistency rules without changing the wire protocol or StorageEngine API.

## Decision

We will provide a move-only, read-only `SessionView::Open` facade that binds to a session generation
using weak StorageNode State and weak overlay owner identity. Each operation enters the node gate,
copies the current reader pair under the session mutex, resolves overlay before snapshot, and
materializes and validates List results before releasing internal synchronization and invoking the
user sink.

## Consequences

* Good: Views cannot retain StorageNode, base engine, snapshot, or overlay resources after their
  owning lifecycle ends.
* Good: Overlay replacement, malformed selected payloads, Stop, and session recreation have
  deterministic Result/Status behavior.
* Good: List callbacks receive owned values outside internal locks and may re-enter the node or call
  Stop without a gate deadlock.
* Bad: SessionView reads copy selected payloads and List materializes the matching parameter set.
* Bad: There is no cross-operation transaction or reader-visible batch isolation; existing
  StorageNode consistency rules remain in force.
* Neutral: Large image and compute-artifact storage remains in the disk-backed `buffers/<sid>/**`
  scope defined by ADR-0014, not in SessionView or ParamCache.

## Options Considered

* **Retain a strong StorageNode State or reader pair** — rejected because a surviving view would
  keep node and session resources alive after Stop or CloseSession.
* **Add a numeric generation counter to StorageNode** — rejected because fresh overlay ownership
  control blocks already provide an ABA-safe generation identity without changing Issue #12 state.
* **Return callback-scoped StorageEngine spans from SessionView** — rejected because the spans
  would dangle after the StorageReader callback returns; zero-copy reads remain ParamCache's role.
* **Invoke List sinks while holding the gate or engine locks** — rejected because sink re-entry
  and sink-triggered Stop could deadlock, and callback views would not have independent ownership.

## References

* Issue #21 / PR #103
* ADR-0014: Session-scoped, disk-backed buffers
* ADR-0017: Atomic StorageNode lifecycle
* ADR-0023: ParamCache consistency and lifetime
* [StorageEngine API](../04_api_cpp.md)
