# ADR-0040: Declare the StorageNode queryable last, as its readiness signal

## Status

Accepted — 2026-10-11

## Context

ADR-0017 made StorageNode start atomic: Start stages the queryable and the subscriber in an
inactive State and activates it only after both declarations succeed. Samples and queries that
arrive while the declarations are staged are rejected, and a successful Start return is the
readiness boundary.

ADR-0038 and ADR-0039 changed what clients treat as readiness. An acknowledged write, and every
client query, now waits until a StorageNode queryable for the prefix is reachable. Only
StorageNode declares queryables, so that match is the only signal a client in another process
has. Under ADR-0017 the signal comes first: Start declares the queryable, then the subscriber,
and activates the State last. A client that matches the queryable during that window can be lost
in two ways:

- A write can reach the node before the subscriber is declared, or while the State is still
  inactive. The writer then times out waiting for its acknowledgement.
- A query can reach the inactive State, which returns with no reply. The client then reports
  "not found".

ADR-0038's Context states that a match means the node's declarations have arrived. That holds only
once this ADR is in effect.

Issue #224 measured this on the published 0.1.2 wheel. A writer was already waiting when a
StorageNode started, with a `ParamCache` attached. One write in 400 timed out.

## Decision

We will declare the StorageNode queryable last, after the subscriber is declared and the State
admits callbacks. The queryable is then the readiness signal that ADR-0038 and ADR-0039 rely on.
This supersedes ADR-0017, whose other decisions are restated in D3.

### D1. Start order

1. Validate the arguments and build the callback-shared State, including any catalog
   reconciliation (ADR-0036).
2. Declare the subscriber. Samples that arrive before step 3 are rejected, as before.
3. Activate the State, so callbacks are admitted.
4. Declare the queryable.
5. Publish the declarations and the State on the node under the lifecycle mutex. `IsStarted`
   becomes true here.

Before step 4, no client that waits on the queryable can see the node. Clients that do not wait
are best-effort, as before.

### D2. Rollback

If the subscriber declaration fails, Start returns its error and nothing is left behind.

If the queryable declaration fails, Start rolls back the way Stop does:

- it closes callback admission and waits for callbacks in flight;
- it releases the Sessions, the catalog, and the acknowledgement and Fence registries;
- it undeclares the subscriber and returns the error.

A later Start can succeed. Samples received between steps 3 and 4 may already have been applied to
the caller's engine. No waiting client can have sent them, and they stay applied.

### D3. Decisions kept from ADR-0017

- Declaration APIs return `Result<Queryable>` and `Result<Subscription>`, so failures keep their
  error codes.
- Lifecycle transactions are serialized. A live StorageNode is not movable.
- A mutex and condition-variable callback gate rejects new callbacks and waits for callbacks in
  flight before the declarations are released. Stop is a deterministic quiescence boundary.
- Caller-supplied Transports stay externally owned and must outlive the node.

### D4. Ordering assumption

A peer receives the declarations of one Zenoh session in the order they are made. A client that
matches the queryable has therefore also received the subscriber declaration, so its write is
routed to the node.

## Consequences

* Good: a write or query from a client that waited for the queryable reaches a node that is ready
  to apply or answer it, including a node that is still inside Start.
* Good: no wire change and no public API change.
* Bad: a Start that fails at the queryable may already have applied samples, so its rollback is no
  longer free of side effects on the engine.
* Neutral: `IsStarted` still turns true only when Start returns success. In-process callers keep
  using the Start return as their readiness boundary.

## Options Considered

* **Keep ADR-0017 and document that Start must return before clients send** — rejected because
  clients in other processes cannot observe that Start has returned. The only signal they have is
  the queryable.
* **Activate before both declarations** — rejected because it adds nothing. No sample can arrive
  before the subscriber is declared.
* **Announce readiness on a separate key** — rejected because it needs a new wire key and a second
  wait in every client, while the queryable already is the signal.

## References

* Issue #224
* [ADR-0017](0017-atomic-storage-node-lifecycle.md) — superseded by this ADR
* [ADR-0038](0038-wait-for-a-storage-node-before-an-acknowledged-write.md) — acknowledged writes
  wait for the queryable
* [ADR-0039](0039-wait-for-a-storage-node-before-a-client-query.md) — client queries wait for the
  queryable
* [ADR-0036](0036-retained-session-catalog.md) — catalog reconciliation at Start
