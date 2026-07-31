# ADR-0032: Define mixed durable and ephemeral session buffer routes

## Status

Accepted — 2026-07-31

## Context

ADR-0014 defined one buffer route and a mutually exclusive persistence mode for each Session.
A computation may need durable outputs and live-only progress or preview data concurrently.
The Session remains the ownership, isolation, and lifecycle boundary, but persistence must be
selected by the route. The wire contract must also interoperate with plain zenoh clients and
must not imply that live delivery and persistence are one atomic operation.

## Decision

We will define the Session buffer routes, value rules, and durable-engine ownership by:

- **Routes** — defining the exact routes `<prefix>/buffers/<sid>/durable/<key>` and
  `<prefix>/buffers/<sid>/ephemeral/<key>` as independent Session capabilities;
- **Capabilities** — allowing each Session to enable either, both, or neither, and preserving both
  `CreateSession` overloads with the one-argument behavior unchanged;
- **Encoding and writes** — using plain `zenoh/bytes` values with no sitos schema or type tag,
  making values immutable and write-once, with same-byte retries idempotent and conflicting PUTs
  protocol-invalid and not persisted;
- **Ephemeral writes** — making ephemeral write-once compliance a publisher obligation without
  retaining ephemeral state;
- **Unsupported operations** — rejecting buffer DELETE, batch, fence, snapshot, and control
  operations in v0.4;
- **Factory transfer** — having `Start` move `DurableBufferEngineFactory` from
  `StorageNodeConfig` into the callback-shared long-lived State before Transport declarations,
  where the State owns it and the factory is invoked at most once per durable-enabled Session;
- **Factory result** — requiring the return type `Result<std::unique_ptr<StorageEngine>>`, and
  having a valid non-null result transfer into unique Session ownership;
- **Engine sharing** — having all durable keys in that Session share its engine, and keeping the
  empty default factory valid when durable capability is not requested;
- **Failure and rollback** — requiring the creation/rollback path to release every attempt-created
  Session record, admission state, and engine state on every non-commit outcome, preserving factory
  `Err` unchanged, containing exceptions as non-OK Results, failing empty and null results, and
  deferring exact Status taxonomy to the Issue #56 scope freeze;
- **First bytes** — leaving the first bytes retained after a conflicting durable PUT.

We will enforce Session lifecycle, admission, and callback synchronization by:

- **Phases and statuses** — enforcing the exact `absent → Creating → Active` and
  `Active → Closing → absent` transitions, mapping creation against `Creating` or `Closing` and
  Close against `Creating` or `Closing` to `std::errc::operation_in_progress`, retaining
  `std::errc::file_exists` for duplicate `Active` creation and
  `std::errc::no_such_file_or_directory` for missing Close, and allowing Close only from `Active`;
- **Reservation** — reserving a non-queryable `Creating` record, committing only the same
  reservation while it remains `Creating`, and removing only that reservation on rollback;
- **Callback gate** — enrolling `CreateSession`, `CloseSession`, and `ActiveSessions` in the node
  callback gate before lifecycle-state access, and having both Transport callbacks take the node
  gate at entry;
- **Active sessions** — having `ActiveSessions` copy only Active SIDs and retain no Session resource
  after returning, and keeping the node gate as the `Stop`/shutdown quiescence boundary;
- **State generation** — using the same callback-shared State generation for `CreateSession`;
- **Subscriber sequencing** — having only the subscriber path hold the global `subscriber_mutex`
  across parsing and application including `durable ReadExact → first-writer decision → Put`;
- **Query isolation** — keeping query callbacks out of `subscriber_mutex`;
- **Admission** — requiring every callback or operation using Session resources to take admission;
- **Lock order** — ordering locks as node gate → `subscriber_mutex` (subscriber only) →
  `session_mutex` → Session admission;
- **Atomic lookup** — making active lookup plus admission atomic under `session_mutex`;
- **External work** — releasing `session_mutex` before external factory or engine operations,
  callback quiescence, destruction, or logging;
- **Phase collisions** — rejecting `Creating`/`Closing` phase collisions without waiting, and
  keeping admission quiescence as a distinct required wait without prescribing condition-variable
  internals;
- **Close ordering** — destroying owned resources before `CloseSession` returns, and keeping the
  SID reserved until release completes;
- **Re-entry** — making the Factory/LogSink caller precondition forbid synchronously calling `Stop`,
  destruction, or another waiting lifecycle operation on the same StorageNode and forbidding
  waiting for a task or thread that does so, while ordinary independent-thread calls and
  non-blocking stop-request posting remain supported.

We will define durable late-join delivery and admission semantics by:

- **Late join** — defining durable late join as buffering subscribe, materialized Get replies,
  buffered-sample drain, and post-transition live delivery in that order, and allowing only
  documented same-byte duplicate handling during the transition;
- **Ephemeral delivery** — keeping ephemeral routes live-only with no initial Get, replay, or
  node-retained state;
- **Fanout** — treating Zenoh fanout and StorageNode persistence as non-atomic;
- **Admission boundary** — treating capability and write-once checks as admission rules rather than
  network ACLs.

## Consequences

* Good: Independent routes allow one Session to combine durable outputs and ephemeral progress
  without a session-wide persistence mode, while the existing one-argument `CreateSession` call
  remains source-compatible.
* Good: Plain byte payloads preserve interoperability and keep receivers byte-opaque; the
  route-selected engine gives durable values one ownership boundary and leaves ephemeral delivery
  live-only.
* Good: Reservation, admission, and callback quiescence prevent resource use after close and make
  same-SID retry possible after a failed creation or completed close.
* Good: Durable late-join evidence must include deterministic sleep-free fake-Transport/barrier
  coverage and a process-isolated raw-Zenoh lane, with #29's topology, readiness, cleanup,
  identifier, and dependency safeguards.
* Bad: Plain bytes provide no schema or type metadata, and Zenoh fanout cannot be retracted when a
  subscriber has already observed a conflicting or invalid publication.
* Bad: Fanout and persistence are not atomic, so duplicate samples and live observations that do
  not match durable Get remain possible; invalid raw publications have no guaranteed
  live-observation semantics, and exactly-once delivery is not promised.
* Bad: Capability and write-once checks do not act as network ACLs, and the synchronous re-entry
  rule for Factory and LogSink callers is a lifecycle safety precondition rather than a runtime
  detection mechanism.
* Neutral: Raw DELETE remains supported for base and session routes; ParamStore Delete remains
  base-only, snapshots remain read-only, and buffer DELETE remains unsupported in v0.4.
* Neutral: Physical directory removal remains host-owned after `CloseSession`; a new v0.4 Session
  receives a fresh or logically empty store. Orderly close/reopen checks cover resource release
  only and do not establish #108 restart or retention semantics; restart catalogs, retention,
  orphan handling, and deletion retry remain #108 responsibilities.
* Neutral: Durability barriers belong to #105, publisher fences to #106, and applied or
  synchronized BufferPublisher fences to #107; #56 adds none of those mechanisms or an
  acknowledgement mechanism.
* Neutral: #56 adds no BufferPublisher, BufferSubscriber, Python buffer, or Python
  engine-factory API and preserves the existing Zenoh-ON/RocksDB-OFF and
  RocksDB-ON/Zenoh-OFF/vcpkg configurations while adding combined
  Zenoh-ON+RocksDB-ON validation on Windows and Linux.
* Neutral: The #56 evidence must cover the approved lifecycle, capability, rollback, ordering,
  and interop matrix; it reuses #29's process-isolation safeguards. Backpressure, chunking,
  shared memory, TTL, history, and generation management remain outside this decision.

## Options Considered

* **One session-wide durable or ephemeral mode** — rejected because one computation needs both
  durable outputs and ephemeral progress data.
* **One unqualified buffer route** — rejected because persistence would remain implicit and would
  not support concurrent route classes.
* **A sitos payload schema or control markers on buffer values** — rejected to preserve plain
  zenoh/bytes interoperability and keep batch, fence, and snapshot concerns separate.
* **Per-key engines or per-key DELETE** — rejected because one Session needs one ownership and
  lifecycle boundary; lifecycle cleanup is sufficient for v0.4.
* **A production BufferSubscriber API** — rejected because the late-join algorithm can be proved
  at the raw transport boundary without adding a high-level API in #56.
* **Restart and retained-session catalogs in this decision** — rejected because #108 owns that
  lifecycle and recovery contract.

## References

* Issue #133 and implementation Issue #56
* Related: ADR-0014 (superseded), ADR-0002, ADR-0003, ADR-0004
* Related responsibilities: Issues #105, #106, #107, and #108
* F10 session resource-release principle applies to the owned durable engine
* [02_architecture.md](../02_architecture.md), [03_wire_protocol.md](../03_wire_protocol.md)
* [04_api_cpp.md](../04_api_cpp.md), [08_contract_registry.md](../08_contract_registry.md)
