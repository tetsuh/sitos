# ADR-0039: Wait for a reachable StorageNode before a client query

## Status

Accepted — 2026-10-10

## Context

ADR-0038 made acknowledged writes wait until a StorageNode queryable for the prefix is reachable,
because a Zenoh session can open before the StorageNode is connected when another peer answers
scouting first. ADR-0038 §D4 left queries unchanged. Issue #223 then showed that the same race
hits the first query after open: with another process keeping a `ParamCache` attached, a
`BufferPublisher` opened right after its session raised `NotFoundError` in 7 of 60 runs, and
`ParamStore.get` failed in 2 of 40 runs (sitos 0.1.1). A query that runs before the node is
connected gets no reply, which sitos reports as "not found" or as an empty result.

Client queries share one shape: a `Transport::Get` bounded by `ClientConfig::query_timeout`. Only
StorageNode declares queryables, so the ADR-0038 hook can tell whether a node for the prefix is
reachable.

## Decision

We will make every client query wait, within its existing query timeout, until a StorageNode
queryable for the prefix is reachable, and then run the query once as before. This extends the use
of the ADR-0038 hook to client queries and narrows ADR-0038 §D4, which left queries unchanged.

### D1. Covered queries

- `ParamStore::Get` (and `Contains`, which uses it) and `ParamStore::List`.
- The `ParamCache` snapshot and overlay queries of `Attach` and of the ADR-0037 rebuild.
- The `BufferPublisher` Open query of `meta/session/<sid>`.

Subscriptions, `SessionView` reads, and acknowledgement polling are unchanged.

### D2. Probe key expression

Every wait uses `<prefix>/meta/ack/*`, the key expression of ADR-0038. Writes and queries for one
prefix therefore share one cached querier in the Zenoh adapter. The probe only detects a reachable
StorageNode; it does not query that key.

### D3. Time budget

The query timeout is the total budget. The wait runs until a match or until the timeout passes; the
query then runs with the time left, but never less than 1 ms, because `Transport::Get` rejects a
non-positive timeout. The wait result does not change what follows: whether the wait matched,
reached the deadline, or failed, the query runs once and its own outcome is reported.

### D4. Scope

A Transport without the ADR-0038 capability keeps today's behavior. StorageNode startup ordering
is tracked separately in #224.

## Consequences

* Good: a query made right after open reaches a StorageNode that is being discovered, instead of
  reporting "not found" or an empty result.
* Good: no wire change, no new public API, and no change to the query results themselves.
* Good: when the node is already connected, the wait is one local status check on a cached querier.
* Bad: when no StorageNode for the prefix is reachable, a query now takes about its full timeout
  before it reports "not found", instead of returning at once.
* Neutral: tests and applications that retry queries until a node appears keep working; they
  succeed on an earlier attempt.

## Options Considered

* **Wait inside `Transport::Get`** — rejected because the adapter does not know the prefix and
  would need a querier per query key expression.
* **Wait once per client at open** — rejected because a client can open before any node starts and
  query later, and a node can restart while the client lives.
* **Document a retry rule only** — rejected because every application would need its own retry
  loop, and `ParamCache.attach` could still bind an empty state without an error.

## References

* Issues #223 and #228
* [ADR-0038](0038-wait-for-a-storage-node-before-an-acknowledged-write.md) — the matching hook and
  acknowledged writes
* [ADR-0037](0037-param-cache-session-liveness-and-recovery.md) — ParamCache rebuild
