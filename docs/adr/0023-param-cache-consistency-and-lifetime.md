# ADR-0023: Define ParamCache consistency and lifetime boundary

## Status

Proposed — 2026-07-19

## Context

ParamCache adds local Result-bearing reads and session-scoped write-through operations. Readers
must remain safe while subscriber callbacks and local writes replace cache values, while Detach
unpublishes active cache State. Write submission can synchronously invoke a subscriber callback,
while delayed self-echoes cannot be identified without out-of-scope publisher or revision
metadata. The ADR process requires an explicit decision for the thread, consistency, and lifetime
model.

## Decision

We will publish immutable ParamCache values through atomic shared State snapshots and quiesce
admitted callbacks and local write operations with one State lease gate. We will submit writes
before applying them locally, serialize local and subscriber mutations per cache with
last-serialized-wins semantics, and retain pre-unpublication State/value snapshots until their
shared owners release them.

## Consequences

* Good: readers retain State and value ownership safely across replacement and Detach without a
  lifecycle mutex or payload copy.
* Good: Detach closes admission, resets the subscription, waits for callback and write leases,
  then atomically unpublishes active State. Reader State/value snapshots acquired before that
  boundary remain immutable and are reclaimed naturally when their shared owners release.
* Good: local writes avoid holding cache locks during Transport submission and preserve a
  deterministic per-cache mutation order.
* Bad: delayed self-echoes are not deduplicated, there is no global writer order, and a
  concurrent reader may observe an intermediate validated batch application.
* Neutral: callers must still externally exclude concurrent destruction or move assignment of the
  ParamCache object itself.

## Options Considered

* **Lock every read through the lifecycle mutex** — rejected because it weakens N01 local-read
  behavior and needlessly serializes readers with lifecycle operations.
* **Optimistically mutate before submission or deduplicate self-echoes** — rejected because a
  failed submission must leave the cache unchanged and no publisher/revision identity exists.
* **Add a StorageEngine transaction or reader-visible batch atomicity** — rejected because it
  changes the existing consistency model beyond Issue #19.

## References

* Issue #19 / PR #102
* Issue #18, Issue #99
* Related: ADR-0017, ADR-0020, ADR-0022
* [ADR process](../10_adr_process.md)
