# ADR-0014: Add a session-scoped, disk-backed buffers key space

## Status

Accepted — 2026-07-09

## Context

Some workloads need to move **large binary values** (from hundreds of kilobytes
to many megabytes each, and many of them per session) from a producer to one or
more consumers, in addition to the small typed parameters that ParamStore and
ParamCache already handle. Two access patterns are required at the same time:

* **Pull**: a late-joining or debugging client fetches a value on demand
  (a zenoh get, or an HTTP GET through the gateway in ADR-0015).
* **Push**: a live consumer that is already subscribed receives the full value
  as soon as it is produced, without a second round trip.

The existing `session/<sid>/**` overlay is the wrong home for these values:
ParamCache subscribes to `session/<sid>/**` and would ingest large binary
payloads into a client-side read cache that is designed for small parameters.
The overlay is also in-memory, whereas large values need bounded memory and
must survive on disk. Total volume per session can reach the gigabyte range.

## Decision

We will introduce an independent top-level scope `<prefix>/buffers/<sid>/<key>`
backed by a **per-session disk StorageEngine** and delivered with a single
`put` that both **fans out the full payload to live subscribers** and **stores
the identical bytes** so a later get returns the same data. Persistence is
selected per session by a `BufferPersistence` mode (`kDurable` = push + store,
`kEphemeral` = push only), defaulting to `kDurable`.

## Consequences

* Good: One `put` serves both push (live subscribers) and pull (get) with no
  double management; the stored bytes and the fanned-out bytes are identical.
* Good: The scope is disjoint from `session/**`, so ParamCache never ingests
  large payloads. It is also disjoint from `base/**` and `snap/**`.
* Good: Values are disk-backed with bounded memory; a session can hold
  gigabytes of buffers.
* Good: Lifetime equals session lifetime — buffers remain get-able from session
  creation until session destruction (`CloseSession`), which purges the buffer
  store (subsequent get returns not-found). "Producer finished" and "session
  destroyed" are distinct events.
* Neutral: Requires a new `KeyKind::Buffer` and key builder/parser branch, plus
  a StorageNode routing branch for `buffers/**`.
* Neutral: `kEphemeral` sessions have no get and no late-join replay by design.
* Trade-off (deferred, not part of this ADR): shared-memory zero-copy for
  same-host transfers, per-subscriber reliability tuning, and backpressure
  policy are left to follow-up ADRs.

## Design notes (normative for the implementation)

* Key space: `<prefix>/buffers/<sid>/<key>`. No `$batch`, no `snap` counterpart.
* `KeyKind::Buffer` added to `key.hpp`; `BuildBufferKey(sid, key)` and a
  `ParseKey` branch; `<sid>` and `<key>` reuse the existing grammar (§1.2).
* Storage: a disk StorageEngine instance is created per session for `kDurable`.
  The engine abstraction is unchanged (Get/List/Put/Delete/TakeSnapshot);
  buffers do not use snapshots.
* Delivery: producer issues `put(buffers/<sid>/<key>, bytes)`. For `kDurable`
  the StorageNode subscriber writes bytes to the session engine; zenoh fans the
  same bytes out to live subscribers. Late joiners use a querying subscriber
  (get `buffers/<sid>/**` then subscribe) — the same loss-prevention ordering
  as `ParamCache::Attach`.
* Mode selection is in-process only (host calls `CreateSession` with
  `SessionOptions{buffers}`, global default via `Config`). **No wire change**:
  the mode is not encoded in keys or payload. Clients that need to know whether
  get is available learn the mode out of band (for the gateway, in the session
  metadata / descriptor).
* `get`/`put`/`delete` for `buffers/**` follow the same fire-and-forget and
  library-API semantics as other scopes.

## Options Considered

* **Reuse `session/<sid>/buffers/**`** — rejected because ParamCache subscribes
  to `session/<sid>/**` and would ingest large payloads into the small-parameter
  read cache.
* **In-memory overlay for large values** — rejected because per-session volume
  reaches the gigabyte range and must be disk-backed with bounded memory.
* **Notify-then-pull (publish only a reference, consumer gets the bytes)** —
  rejected because consumers require direct full-payload push without a second
  round trip; the chosen design keeps pull available as well.
* **Independent `buffers/<sid>/**` scope with a per-session disk engine** —
  selected; disjoint from parameter scopes, disk-backed, single-put push+store.

## References

* Related: ADR-0002 (embedded storage node), ADR-0003 (engines), ADR-0004
  (snapshots — explicitly not used by buffers)
* Docs: `docs/02_architecture.md` §2, `docs/03_wire_protocol.md` §1
* Issue: #56 (implementation)
