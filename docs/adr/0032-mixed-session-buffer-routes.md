# ADR-0032: Define mixed durable and ephemeral session buffer routes

## Status

Proposed — 2026-07-30

## Context

ADR-0014 defined one buffer route and a mutually exclusive persistence mode for each Session.
A computation may need durable outputs and live-only progress or preview data concurrently.
The Session remains the ownership, isolation, and lifecycle boundary, but persistence must be
selected by the route. The wire contract must also interoperate with plain zenoh clients and
must not imply that live delivery and persistence are one atomic operation.

## Decision

We will define durable and ephemeral buffer routes as independent Session capabilities.
We will use plain `zenoh/bytes` values with one host-created durable engine per enabled Session.
We will use reservation, admission, and callback-quiescence rules to make Session lifecycle safe.

## Consequences

* Good: One Session can publish durable and ephemeral values without a session-wide mode.
* Good: The routes are `<prefix>/buffers/<sid>/durable/<key>` and
  `<prefix>/buffers/<sid>/ephemeral/<key>`. A Session may enable either, both, or neither through
  `SessionOptions`; the existing `CreateSession(sid)` enables neither and remains source-compatible.
* Good: Buffer payloads are plain `zenoh/bytes` with no sitos schema or type tag. Keys are immutable
  and write-once: identical repeated PUTs are idempotent, while conflicting PUTs are
  protocol-invalid and not persisted. Ephemeral publishers follow the same contract, but no
  ephemeral state is retained to enforce it.
* Good: DELETE, `:batch`, `:fence`, snapshots, and control namespaces are unsupported in v0.4.
* Good: Durable late join is the normative buffering-subscriber → synchronous Get/materialize →
  drain under one ordering boundary → live algorithm. Ephemeral routes have no replay or initial
  Get requirement, and StorageNode retains no ephemeral state. A production BufferSubscriber API is
  not added; process-isolated raw-Zenoh tests verify the algorithm.
* Good: A node-level host factory returning `Result<std::unique_ptr<StorageEngine>>` creates at
  most one durable engine per Session. The factory has an empty default in `StorageNodeConfig`; its
  result is uniquely owned by the Session, and all durable keys share it.
* Good: Session phases are exact: creation is `absent → Creating → Active`, and close is
  `Active → Closing → absent`. Creation against `Creating` or `Closing` returns
  `std::errc::operation_in_progress`; an `Active` duplicate retains
  `std::errc::file_exists`. Close against `Creating` or `Closing` returns
  `std::errc::operation_in_progress`, and missing Close retains
  `std::errc::no_such_file_or_directory`.
* Good: Creation reserves a non-queryable `Creating` record before external engine creation. The
  creator commits only after verifying under `session_mutex` that the same reservation still exists
  and remains `Creating`; only `Active` records are queryable. Every resource already created is
  rolled back on failure, and the reservation is removed.
* Good: Each Session has an admission/quiescence gate covering every callback or operation that can
  use Session resources. The lock order is node gate → `subscriber_mutex` (subscriber only) →
  `session_mutex` → Session admission. Both Transport callbacks acquire the node gate at entry;
  subscriber parsing and application, including durable ReadExact → first-writer decision → Put,
  is under `subscriber_mutex`. Query callbacks stay out of `subscriber_mutex`. Active lookup and
  admission are atomic under `session_mutex`, which is released before engine work or logging.
* Good: Close changes `Active → Closing`, rejects same-SID recreation, and waits for admitted
  callbacks and operations to quiesce, destroys the durable engine, releases all other resources,
  and returns
  only after that ownership ends. `session_mutex` is not held during external factory or engine
  operations, callback quiescence, destruction, or logging. Same-SID lifecycle phase collisions do
  not wait and return `std::errc::operation_in_progress`; admission quiescence is a separate
  required wait and may use the gate's normal synchronization implementation. This contract does
  not prescribe condition-variable internals.
* Neutral: The node-wide callback gate remains responsible for StorageNode Stop/shutdown. The global
  subscriber sequencing boundary remains. Orderly engine close/reopen checks validate resource
  release only; they do not establish #108 restart or retention semantics. Physical directory
  removal is host-owned after CloseSession returns, and a new v0.4 Session gets a fresh or logically
  empty store. #108 owns restart catalogs, retention, orphan handling, and deletion retry.
* Bad: StorageNode is one subscriber and cannot retract a sample already observed by another
  subscriber. A durable conflicting raw PUT may therefore be observed live even though durable Get
  keeps the first bytes; consumers must treat the traffic as protocol-invalid.
* Bad: Zenoh fanout and StorageNode persistence are not atomic. Invalid raw publications have no
  guaranteed live-observation semantics, duplicate samples may occur, and exactly-once delivery is
  not promised.
* Bad: Capability checks and write-once validation are admission rules, not network ACLs, and do
  not control other subscribers' observations.
* Neutral: Applied and synchronized fences belong to #107, using #105 and #106 mechanisms; #56
  remains unacknowledged. #56 also adds no BufferPublisher, BufferSubscriber, or engine-factory API
  for Python. #56 must preserve existing Zenoh-ON/RocksDB-OFF and RocksDB-ON/Zenoh-OFF/vcpkg
  configurations while adding combined Zenoh-ON+RocksDB-ON validation on Windows and Linux. Its
  interop tests reuse #29's one-Transport/session topology as applicable, bounded readiness and
  command handshakes, failure-safe cleanup, unique identifiers, and hash-locked Python
  dependencies. This is an #56 implementation requirement, not this documentation change.
* Neutral: Backpressure, chunking, shared memory, retention, TTL, history, and generation
  management remain outside this decision.

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
