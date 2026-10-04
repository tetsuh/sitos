# ADR-0037: Detect Session loss with liveliness tokens and rebuild ParamCache automatically

## Status

Proposed — 2026-10-04

## Context

A `ParamCache` copies one Session's snapshot and overlay at `Attach` and then follows live samples.
When the StorageNode that serves the Session stops, crashes, or becomes unreachable, the cache keeps
its last-known values and cannot tell that they may be outdated. When the Session is created again,
the cache never converges to it. Issue #20 requires a stale indication, reads that stay available
while stale, and an automatic rebuild, and it forbids polling and inference from error messages.
The current contracts give the cache no signal: `Transport` exposes no liveness event, a cache
`Put` is a fire-and-forget publication, and reads do not use the Transport. A Transport callback
must not wait ([02] §9), so a liveness callback cannot run the blocking `Get` calls of a rebuild.
zenoh-c 1.9.0, the pinned version, provides liveliness tokens and subscribers in its stable API;
Zenoh withdraws a token when the declaring session undeclares it, closes, or is lost.

## Decision

We will have StorageNode declare one Zenoh liveliness token for each active Session, keyed by the
Session's `sid` and existing `generation_uuid`. An attached `ParamCache` will subscribe to the
tokens of its `sid`, report `IsStale()` while it is not bound to a live generation, and rebuild
itself on a dedicated worker thread with the existing Attach sequence. The stale flag is advisory:
read and write behavior does not change while stale.

The subsections record the owner decisions DEC-20-001 to DEC-20-009 on Issue #20.

### D1. Failure model (DEC-20-001)

- This ADR covers the loss and re-creation of the Session that serves the attached `sid`. The
  cause may be a StorageNode stop, a crash, a `CloseSession` or `RetainSession`, or a lost path
  between the cache and the node.
- Loss and reopening of the cache's own Transport is out of scope.
- The StorageNode is authoritative. A rebuild replaces the cache contents with the node's snapshot
  and overlay. A value that exists only in the cache, for example one written while no Session was
  serving, is not kept.
- One `sid` under one prefix served by more than one StorageNode at a time is not supported.

### D2. Session liveliness token (DEC-20-002)

- The token key is `<prefix>/meta/live/session/<sid>/<generation_uuid>`. `<generation_uuid>` is
  the same canonical lowercase UUIDv4 text that `meta/session/<sid>` already serves (ADR-0035).
  The token carries no payload. Ordinary subscribers and queryables do not receive tokens.
- StorageNode declares the token only for a Session whose lifecycle is `active`, and only after
  the Session serves `snap/<sid>` and `session/<sid>` reads. A `retained`, `orphaned`, `deleting`,
  or `delete_failed` Session (ADR-0036) has no token.
- StorageNode withdraws the token when the Session stops being active: in `CloseSession`, in
  `RetainSession`, and in `Stop`. A lifecycle call that fails and leaves the Session active keeps
  the token. Zenoh withdraws the token when the node's session closes or is lost.
- The declaration and the withdrawal of one Session generation are serialized with each other, so
  no token outlives the active state of its Session, even when `CreateSession` races
  `CloseSession`, `RetainSession`, or `Stop`.
- A subscriber callback can run synchronously inside the declaration and the withdrawal.
  StorageNode therefore declares and withdraws tokens without holding the locks that guard the
  Session table or request handling.
- A token declaration that fails is logged and does not fail `CreateSession`. Caches of that
  Session then report stale and do not recover automatically.

### D3. Transport liveliness hooks (DEC-20-002)

- `Transport` gains an optional capability, following the `SupportsFenceProfile` pattern:
  a `SupportsLiveliness()` query that returns false by default, a token declaration, and a
  liveliness subscriber declaration that always requests history. The default declarations return
  `InvalidArgument`. Existing custom Transports stay source compatible.
- The Zenoh Transport implements the hooks with the stable liveliness API. Raw Zenoh calls stay
  under `src/transport/`.
- A liveliness subscriber receives the token key and whether the token appeared (Put) or
  disappeared (Delete).
- On a Transport without the capability, StorageNode declares no token, and ParamCache declares no
  liveliness subscriber, starts no worker, and never reports stale. Its behavior is unchanged from
  today.

### D4. Cache rules (DEC-20-002, DEC-20-003)

The cache tracks which generations of its `sid` are live. It is bound to at most one generation:
the generation that the D6 check verified its state against. An attached cache that is not bound
is stale.

1. A Delete of the bound generation unbinds the cache.
2. A Put of a generation other than the bound one unbinds the cache.

While the cache is stale and at least one generation is live, the worker rebuilds the cache toward
the most recently announced live generation. No other event changes the binding: a Put of the
bound generation is ignored, and a Delete of another generation only removes it from the live
generations.

Recovery is driven by these events only. The cache does not poll the node and does not inspect
error messages.

### D5. Liveliness subscriber and recovery worker (DEC-20-004)

- On a liveliness-capable Transport, `Attach` declares one liveliness subscriber for
  `<prefix>/meta/live/session/<sid>/*` before its first `Get` and starts one recovery worker
  thread. Both belong to the cache, not to a candidate state, and live until `Detach`. A rebuild
  does not declare the liveliness subscriber again.
- Liveliness callbacks only record the event and wake the worker. They never call `Get`.
- The worker is the only caller of the rebuild. Events that arrive during a rebuild are coalesced:
  the worker re-evaluates D4 after each attempt.

### D6. Rebuild and swap (DEC-20-005)

- A rebuild runs the Attach sequence of [02] §5.1 into a new candidate state: declare the
  `session/<sid>/**` and Fence-marker subscribers, read the snapshot, read the overlay, apply the
  buffered samples, and go live. The loss-prevention sequence itself does not change.
- The worker binds the candidate to the target generation only if the generation was announced
  before the first `Get` of the rebuild and no liveliness event for the `sid` arrived between that
  `Get` and the swap. Otherwise it discards the candidate and re-evaluates D4. The check, the
  swap, and the bind are one atomic step with respect to liveliness callbacks. This check replaces
  a second read of `meta/session/<sid>`.
- The swap stores the candidate as the active state atomically and then quiesces the replaced
  state as `Detach` does. Until the swap, the old state stays readable and keeps applying live
  samples. Reads never block on recovery and never observe a partly built state.
- A rebuild creates a new attach generation (ADR-0029). Operations in flight on the replaced state
  complete as they do when they race `Detach`; an admitted local-delivery waiter completes with
  `Disconnected`. A call made after the swap uses the new state.
- Convergence is eventual. Liveliness events are asynchronous, so the binding can lag behind a
  Session change by the delivery delay of the events. How long Zenoh takes to withdraw the token of
  a crashed or unreachable node depends on the Zenoh configuration and is not bounded by sitos.

### D7. Attach, writes, and reads while stale (DEC-20-006)

- The success and failure conditions of `Attach` are unchanged, with one addition: on a
  liveliness-capable Transport, a failed liveliness subscriber declaration or a failed worker start
  fails `Attach` like the other declarations and leaves the cache detached.
- `Attach` applies the D6 binding check to the state it read. When no generation was announced
  before its first `Get`, or an event arrived during the sequence, `Attach` still succeeds, the
  cache starts stale, and the worker rebuilds it once a generation is live.
- `Put`, `PutBatch`, and `WaitForLocalDelivery` are admitted while the cache is stale exactly as
  they are when it is not. A write made while stale can be replaced by the next rebuild (D1).
- Reads return last-known values while stale, with the same results and cost as today.

### D8. Retry, `Detach`, and shutdown (DEC-20-007)

- A failed rebuild leaves the cache stale and the old state active. The worker retries after a
  fixed interval while a generation is live. The interval is an internal constant, not public
  configuration. A liveliness event or `Detach` ends the wait early.
- `Detach`, destruction, and move assignment undeclare the liveliness subscriber, stop the worker,
  join it, and then run the existing quiescence. Concurrent `Detach` callers stay serialized. The
  worker never waits for a lock that `Detach` holds while it joins, and it checks the stop request
  before each `Get`. An in-flight `Get` cannot be cancelled, so `Detach` can block for up to one
  `ClientConfig::query_timeout`. `Detach` stays `noexcept`.
- Move construction transfers the subscriber and the running worker together with the state.

### D9. Public API (DEC-20-008)

- `ParamCache` gains `bool IsStale() const noexcept`. It returns true while the cache is attached
  and not bound (D4). It returns false for a detached or moved-from cache and on a Transport
  without liveliness.
- No read or write signature changes, and no `Status` value is added.
- The Python surface is unchanged by this ADR. A Python `stale` property is a separate Issue.

### D10. Scope (DEC-20-009)

Issue #20 implements this ADR in one PR: the token, the Transport hooks, the cache rules, the
worker, `IsStale()`, and the documentation of the key list, the thread model, and the cache
contract.

## Consequences

* Good: a live cache learns that its Session is gone and converges to the re-created Session
  without polling, and without any call from the application.
* Good: reads stay local and non-blocking; the read hot path gains no work (N01, N07).
* Good: the token key reuses `generation_uuid`, so a re-created Session and a restored path to the
  same Session are handled by the same two rules.
* Good: the rebuild reuses the Attach sequence, so recovery has the same loss-prevention guarantee
  as `Attach`.
* Bad: the wire gains one liveliness key under `meta/`. Older caches ignore it. A new cache
  attached to a node that declares no token reports stale permanently, but reads and writes work
  as before.
* Bad: each attached cache owns one more thread, and `Detach` can block for up to one
  `query_timeout`.
* Bad: an `Attach` whose token history arrives after its first `Get` performs one additional
  rebuild, so it reads the Session twice.
* Bad: a write made while stale can be lost at the next rebuild, and the caller gets no error.
  Applications that need to know check `IsStale()`.
* Bad: a write or a local-delivery wait that races a swap can fail as it does when it races
  `Detach`, although the application did not detach. A retry uses the new state.
* Bad: a cache attached to a `sid` whose Session is retained, orphaned, or absent stays stale
  until an active Session with that `sid` is created.
* Bad: as with `Attach` today, a rebuild cannot tell an empty Session from a Session whose reads
  are not routable yet; zero replies build an empty state. If Zenoh announces a token before the
  node's queryable is reachable, the cache binds an empty state until the next liveliness change.
* Neutral: `Transport` gains optional hooks; a Transport without them keeps today's behavior.
* Neutral: Python caches recover in the same way without a new Python API.

## Options Considered

* **Reuse `meta/session/<sid>` instead of a new key** — rejected because it is a query surface.
  A cache could learn of a re-created Session only by polling it, which Issue #20 forbids. The
  token reuses that surface's `generation_uuid` and adds only the notification.
* **One token per StorageNode** — rejected because a returning node does not imply a re-created
  Session, and waiting for the Session would need polling.
* **Querier matching status** — rejected because the StorageNode queryable covers the whole
  prefix, so matching cannot identify a Session or its generation.
* **Zenoh transport and link events** — rejected because they are outside the stable zenoh-c API.
* **A public `Reattach()` called by the application** — rejected because Issue #20 requires an
  automatic rebuild, and the application would still need the same signal to know when to call it.
* **Rebuild in the liveliness callback** — rejected because a Transport callback must not wait
  ([02] §9), and a rebuild runs blocking `Get` calls (ADR-0020).
* **Reject writes with `Disconnected` while stale** — rejected to keep the flag advisory. It would
  add a failure mode to three calls and would change today's behavior for a cache attached before
  its Session exists.
* **Read `meta/session/<sid>` before and after the rebuild to bind the generation** — rejected
  because the token key already names the generation, and the D6 event check covers a change
  during the rebuild.
* **Exponential backoff or a public retry setting** — rejected because one fixed internal interval
  is enough while the retry is bounded by the token's lifetime.
* **A stale indication in every read result** — rejected because it changes every read signature
  and adds work to the read hot path.
* **Freeze the old state while stale** — rejected because it needs extra machinery and stops
  updates after a false alarm such as a short path loss.

## References

* Issue #20 and owner decision comment `issuecomment-5975113694`; ADR prerequisite Issue #199
* Related: ADR-0020 (blocking `Get` completion), ADR-0029 (attach generation and Fence lanes),
  ADR-0035 (`generation_uuid`), ADR-0036 (Session lifecycle states)
* [02] §5.1, §9, §10; [03] §1, §7.1
* Contract registry row: Session liveliness token
