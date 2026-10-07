# ADR-0038: Wait for a reachable StorageNode before an acknowledged write

## Status

Proposed — 2026-10-10

## Context

An acknowledged `Put` or `PutBatch` (ADR-0028) submits its data once and then polls
`<prefix>/meta/ack/<token>` until the total deadline. Issue #217 found that the submission itself
can be lost. With the default Zenoh 1.9.0 configuration (multicast scouting and
`open.return_conditions.connect_scouted: true`), session open returns once every peer connector
known so far has finished. When another peer, such as a process with an attached `ParamCache`,
answers scouting first and its connection completes before the StorageNode's hello is handled,
open returns without a link to the node. Peers do not forward for each other, so a sample sent
right after open reaches only that peer. The node never sees the token, the client polls until the
deadline, and the caller gets `Timeout` for a write that was never applied.

The same race explains the Linux wheel failure in `tests/python/test_storage_node_live.py`, where a
store worker opens its session while the node worker is still starting. ADR-0028 forbids
resubmitting data, so a retry after the loss is not an option. A fixed sleep after open would delay
every client and still not bound the race.

Only StorageNode declares queryables, under `<prefix>/**`. A queryable that matches
`<prefix>/meta/ack/*` is therefore visible exactly when a node for that prefix is connected and its
declarations have arrived. zenoh-c 1.9.0 reports this through a querier's matching status in its
stable API. Publisher matching cannot be used, because an attached `ParamCache` also subscribes to
Session keys.

## Decision

We will make an acknowledged write wait, within its existing total deadline, until a queryable
matches its acknowledgement key expression, and then submit the data once as before.

### D1. Transport capability

`Transport` gains one optional hook:

```cpp
virtual Result<bool> WaitForMatchingQueryable(std::string_view keyexpr,
                                              std::chrono::steady_clock::time_point deadline);
```

It returns `true` once a queryable that matches `keyexpr` is reachable, and `false` when `deadline`
passes first. The default returns `true` at once, so a Transport that does not provide the
capability keeps today's behavior and existing custom Transports compile unchanged.

### D2. Zenoh implementation

The Zenoh adapter declares one querier per key expression on first use, keeps it for the session's
lifetime, and polls `z_querier_get_matching_status` every 10 ms until it matches or the deadline
passes. The queriers are dropped before the session closes. The wait sends no sample and changes no
key, Encoding, attachment, or payload.

### D3. Acknowledged submission

`SubmitAcknowledgedWrite` calls the hook with `<prefix>/meta/ack/*` and the operation's total
deadline after it validates its inputs and generates the token, and before its single submission.
The result of the wait does not change what follows: whether the wait matched, reached the
deadline, or failed, the data is submitted exactly once and the acknowledgement is polled until the
same total deadline. A write therefore never fails earlier than it does today, and ADR-0028's
at-most-once submission and result semantics are unchanged.

### D4. Scope

- Acknowledged `Put` and `PutBatch`, from C++ and Python, use the wait.
- Unacknowledged writes, `Delete`, `Get`, and subscriptions are unchanged.
- The Fence marker submission (ADR-0029, ADR-0035) has the same race class and is left to a
  follow-up.

## Consequences

* Good: an acknowledged write made right after open reaches the StorageNode once the node is
  connected, instead of timing out without being applied.
* Good: no wire change, and no change to the acknowledgement protocol or its results.
* Good: when the node is already connected, the wait is one local status check on a cached querier.
* Bad: `Transport` gains another optional public hook.
* Bad: each prefix used for acknowledged writes keeps one querier for the session's lifetime.
* Bad: when no node for the prefix is reachable, the wait consumes the total deadline before the
  single submission, so a node that connects late in the deadline leaves little time to poll.
* Neutral: a Transport without the capability, and any write while no node is reachable, behave as
  they do today.

## Options Considered

* **Resubmit the data with the same token** — rejected because ADR-0028 submits data at most once,
  and the node's duplicate suppression is bounded and lost on restart.
* **Sleep after session open, or raise `scouting.delay`** — rejected because it delays every client
  and still does not bound the race; open already returns within milliseconds with the defaults.
* **Disable `connect_scouted` or change other Zenoh open conditions** — rejected because the default
  configuration is what users get, and no open condition waits for peers that have not answered yet.
* **Wait in `ParamStore::Open`** — rejected because a store can open before any node starts, as in
  the live test; the race must be closed at the write.
* **Publisher matching status** — rejected because an attached `ParamCache` subscribes to Session
  keys and would satisfy it without a node.
* **Wait inside the Zenoh adapter's `Put`** — rejected because the adapter does not know the
  operation's deadline or the prefix's acknowledgement key, and would need a fixed cap and a
  querier per data key.

## References

* Issue #217 and its diagnosis comment
* [ADR-0028](0028-unify-acknowledged-operation-results.md) — acknowledged operation results
* [ADR-0037](0037-param-cache-session-liveness-and-recovery.md) — the previous optional Transport
  capability
* [ADR-0013](0013-default-to-zenoh-scouting-with-explicit-endpoint-override.md) — default
  Zenoh scouting
