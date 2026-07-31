# ADR-0032: Define mixed durable and ephemeral session buffer routes

## Status

Proposed — pending owner acceptance immediately before merge

## Context

ADR-0014 defined one buffer route and a mutually exclusive persistence mode for each Session.
A computation may need durable outputs and live-only progress or preview data concurrently.
The Session remains the ownership, isolation, and lifecycle boundary, but persistence must be
selected by the route. The wire contract must also interoperate with plain zenoh clients and
must not imply that live delivery and persistence are one atomic operation.

## Decision

We will define the exact routes `<prefix>/buffers/<sid>/durable/<key>` and
`<prefix>/buffers/<sid>/ephemeral/<key>` as independent Session capabilities, with each Session
able to enable either, both, or neither, preserve both `CreateSession` overloads with the
one-argument behavior unchanged, use plain `zenoh/bytes` values with no sitos schema or type tag,
make values immutable and write-once, with same-byte retries idempotent and conflicting PUTs
protocol-invalid and not persisted, make ephemeral write-once compliance a publisher obligation
without retaining ephemeral state, reject buffer DELETE, batch, fence, snapshot, and control
operations in v0.4, and have `Start` move `DurableBufferEngineFactory` from
`StorageNodeConfig` into the callback-shared long-lived State before Transport declarations,
where the State owns it and the factory is invoked at most once per durable-enabled Session;
its return type is `Result<std::unique_ptr<StorageEngine>>`, and a valid non-null result
transfers into unique Session ownership; all durable keys in that Session share its engine; the
empty default factory remains valid when durable capability is not requested; the creation/rollback
path releases every attempt-created
Session record, admission state, and engine state on every non-commit outcome, preserves factory
`Err` unchanged, contains exceptions as non-OK Results, fails empty and null results, defers
exact Status taxonomy to the Issue #56 scope freeze, and leaves the first bytes retained after a
conflicting durable PUT.
We will enforce the exact `absent → Creating → Active` and `Active → Closing → absent` transitions,
map creation against `Creating` or `Closing` and Close against `Creating` or `Closing` to
`std::errc::operation_in_progress`, retain `std::errc::file_exists` for duplicate `Active` creation
and `std::errc::no_such_file_or_directory` for missing Close, allow Close only from `Active`,
reserve a non-queryable `Creating` record, enroll `CreateSession`, `CloseSession`, and
`ActiveSessions` in the node callback gate before lifecycle-state access, have `ActiveSessions` copy
only Active SIDs and retain no Session resource after returning, keep the node gate as the
`Stop`/shutdown quiescence boundary, use the same callback-shared State generation for
`CreateSession`, commit only the same reservation while it remains `Creating`, remove only that
reservation on rollback, have both Transport callbacks take the node gate at entry, have only the
subscriber path hold the global `subscriber_mutex` across parsing and application including
`durable ReadExact → first-writer decision → Put`, keep query callbacks out of
`subscriber_mutex`, require every callback or operation using Session resources to take admission,
order locks as node gate → `subscriber_mutex` (subscriber only) → `session_mutex` → Session
admission, make active lookup plus admission atomic under `session_mutex`, release `session_mutex`
before external factory or engine operations, callback quiescence, destruction, or logging, reject
`Creating`/`Closing` phase collisions without waiting, keep admission quiescence as a distinct
required wait without prescribing condition-variable internals, destroy owned resources before
`CloseSession` returns, keep the SID reserved until release completes, and make the Factory/LogSink
caller precondition forbid synchronously calling `Stop`, destruction, or another waiting lifecycle
operation on the same StorageNode and forbid waiting for a task or thread that does so, while
ordinary independent-thread calls and non-blocking stop-request posting remain supported.
We will define durable late join as buffering subscribe, materialized Get replies, buffered-sample
drain, and post-transition live delivery in that order, allow only documented same-byte duplicate
handling during the transition, keep ephemeral routes live-only with no initial Get, replay, or
node-retained state, treat Zenoh fanout and StorageNode persistence as non-atomic, and treat
capability and write-once checks as admission rules rather than network ACLs.

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
* Neutral: #56 adds no BufferPublisher, BufferSubscriber, Python buffer, or engine-factory API and
  preserves the existing Zenoh-ON/RocksDB-OFF and RocksDB-ON/Zenoh-OFF/vcpkg configurations while
  adding combined Zenoh-ON+RocksDB-ON validation on Windows and Linux.
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
