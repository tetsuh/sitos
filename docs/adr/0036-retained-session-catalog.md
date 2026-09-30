# ADR-0036: Persist a retained-session catalog and reconcile it at StorageNode start

## Status

Accepted — 2026-09-30

## Context

ADR-0032 keeps durable session buffers in one host-supplied engine per Session and leaves directory
removal to the host. Session metadata, snapshots, overlays, and engine handles live only in
StorageNode memory, so after a host restart durable data may remain on disk with no record with
which sitos can reopen, serve, or delete it. Hosts also need to keep a finished Session's durable
buffers for clients and delete them later with `CloseSession`, across restarts, without restoring
compute. Issue #108 fixes the boundaries: retention is not a write barrier, compute is never
resumed, and catalog corruption latches readiness false until offline repair and restart.
ADR-0033 requires every snapshot owner to be released before a RocksDB directory can be removed on
Windows, and ADR-0034 leaves the catalog crash points to #108.

## Decision

We will add an opt-in, library-owned durable root in which one exclusive RocksDB catalog records
each durable-buffer Session's lifecycle and generation-scoped store directory, make every catalog
transition disk-synchronized before it becomes visible, reconcile the catalog at `Start` into
retained, orphaned, and resumed-deletion states without restoring volatile state, and report
lost volatile state and an unavailable catalog as typed results, with a monotonic readiness latch.

### D1. Ownership and layout

- `StorageNodeConfig` gains `std::optional<std::filesystem::path> durable_root`. When it is set
  (catalog mode), StorageNode owns the layout and opens durable stores with `RocksDBEngine::Open`;
  `durable_buffer_engine_factory` must then be empty (`InvalidArgument` at `Start`). Catalog mode
  requires a RocksDB-enabled build. Without `durable_root`, ADR-0032 behavior is unchanged and has
  no restart semantics.
- Layout under `durable_root`:
  - `catalog/` — the catalog RocksDB instance;
  - `sessions/<sid>/<generation_uuid>/` — one durable store per Session incarnation.
- The `generation_uuid` directory component is the generation rule: cleanup of an old
  incarnation addresses only its own directory and can never remove a recreated Session's store.

### D2. Exclusive ownership and instance identity

- The catalog's RocksDB `LOCK` gives single-process exclusive ownership of `durable_root`.
- Each successful `Start` generates a fresh UUIDv4 host instance id and records it in the catalog
  before serving. Because ownership is exclusive, every `active` record found at `Start` belongs to
  a previous instance.

### D3. Catalog schema

- Keys: `schema` → `{"schema_version":1}`; `instance` → current instance id and start time;
  `session/<sid>` → one versioned record.
- Record fields: `schema_version`, `sid`, `generation_uuid`, `state`, `owner_instance_id`,
  `routes` (`durable`, `ephemeral`), `created_at`, and when applicable `retained_at`,
  `orphaned_at`, `deleting_at`, `deleted_at`, and `failure` (sanitized: operation, error category,
  platform error code; never paths, parameter values, payloads, or application metadata).
- Ephemeral-only Sessions have no record (ephemeral exclusion).
- First use: when `catalog/` does not exist and `sessions/` is absent or empty, `Start` creates
  the catalog and writes `schema` and `instance` in one synchronized batch before validation. When
  `catalog/` does not exist but `sessions/` holds any entry, the catalog is missing, not new, and
  `Start` latches (D8) instead of creating one. An existing catalog is opened without
  `create_if_missing`, and startup never writes a missing `schema` key into it.
- Migration: v0.5 defines schema 1 only. In an existing catalog, a missing, unknown, or newer
  `schema_version`, or any record that fails validation, is a validation failure (D8). A future
  schema change requires a new ADR that defines its migration.

### D4. Transitions, linearization, and disk-sync points

Every catalog write uses a synchronized write (`WriteOptions::sync = true`). An operation's
linearization point is the successful return of its catalog write; a failed write leaves the prior
durable record authoritative, fails the operation, and latches the catalog unavailable (D8).

| Operation | Order |
|---|---|
| `CreateSession` (durable route) | write `active` record → create store directory and open engine → activate. If directory creation or `RocksDBEngine::Open` fails in the running process, the call fails with that error and the node runs the `CloseSession` deletion order below on the new generation (`deleting` → remove any partial directory → `deleted`, or `delete_failed`); it neither erases the record nor latches unless a catalog write fails. A crash after the `active` write instead leaves a record without a store, which reconciliation makes `orphaned` with an empty store. |
| `RetainSession` | `StorageEngine::Sync` on the durable store → write `retained` → release snapshot and overlay. A Sync failure fails the call with no state change. |
| `CloseSession` (durable route) | write `deleting` → reject new admissions and quiesce (existing `BeginClose`, fence `CloseAndWait`, `WaitForAdmission`) → release every engine and snapshot owner → remove the generation directory → write `deleted`. A removal failure writes `delete_failed` with sanitized diagnostics. |

### D5. Retain API and runtime effect

- `Result<SessionLifecycleState> StorageNode::RetainSession(std::string_view sid)` (C++ only in
  v0.5). Allowed from `active` on a Session with the durable route; repeated calls on `retained`
  return `retained` without writing. Ephemeral-only Sessions return `InvalidArgument`; unknown
  Sessions return `NotFound`; `deleting`, `delete_failed`, and `orphaned` return
  `InvalidArgument`.
- Retain does not stop the node's durable or ephemeral buffer receivers or fence dispatch in the
  running process: it is neither a publisher drain nor a write barrier.
- After restart, `retained` and `orphaned` stores are reopened for durable Get/List and
  `CloseSession` only; no buffer receivers, fence dispatch, snapshot, overlay, ParamCache state,
  or compute are recreated.

### D6. Typed results and their wire form

- `Status` appends `StateLost = 10` and `CatalogUnavailable = 11`; Python adds
  `StateLostError` and `CatalogUnavailableError` under `SitosError`.
- The storage node answers a query it must refuse with a Zenoh error reply carrying
  `{"v":1,"status":<n>}`. `TransportQuery::ReplyError` is added inside `src/transport/`, and a
  client `Get` that receives such a reply returns that Status (any other error reply stays
  `Error`). The two values are added to the ADR-0028 wire allowlist.
- `StateLost`: session-scoped parameter reads (`snap` and `session` routes) of a `retained` or
  `orphaned` Session. `NotFound` keeps meaning an absent key or an unknown Session.
- `meta/session/<sid>` reports `retained`, `orphaned`, `deleting`, and `delete_failed` in its
  existing `state` field.

### D7. Delete gates and Windows-safe removal

- Existing admission and fence quiescence gate the deletion; no new drain is introduced.
- The engine and all snapshot owners are released before `std::filesystem::remove_all` on the
  generation directory (ADR-0033). A failure is not retried in process; it records
  `delete_failed`, and an explicit `CloseSession` retries from `deleting`.
- At `Start`, a `deleting` record resumes automatically (a durable administrative request);
  `delete_failed` waits for an explicit `CloseSession`.
- Tombstones: a `deleted` record remains until the same `sid` is created again, which overwrites
  it with a new generation. `CreateSession` on a `retained`, `orphaned`, `deleting`, or
  `delete_failed` sid returns the existing-session error.

### D8. Catalog unavailability, readiness latch, and gate

- Catalog open failure (including a held `LOCK`), validation failure, or a failed catalog write
  latches the process-wide catalog-unavailable condition. `Start` still succeeds so the host stays
  live.
- `StorageNode::Readiness()` returns `{ready, reason}`; `ready` is false once the latch is set and
  never returns to true in the same process. Sanitized diagnostics go to the log sink.
- One shared gate rejects every catalog-dependent operation with `CatalogUnavailable`:
  `CreateSession` (all Sessions), `RetainSession`, `CloseSession`, and durable-buffer Get/List
  (error reply). Base routes and ephemeral fanout are not catalog-dependent.
- The node never repairs, regenerates, or replaces the catalog and never deletes unknown
  directories.

### D9. Reconciliation at `Start`

1. Open and validate the catalog; on failure, latch (D8) and skip the remaining steps.
2. Record the new instance id.
3. Scan `sessions/`; directories without a matching `(sid, generation_uuid)` record are logged as
   unknown and never served or removed.
4. Durably turn previous-instance `active` records into `orphaned`.
5. Keep `retained` and `orphaned` records, and reopen their stores for Get/List only (D5).
6. Resume `deleting` records (D7).
7. Serve no volatile state for any reconciled Session.

### D10. Scope

- C++ hosting only; Python gains the new exception classes for client reads but no catalog
  hosting or `RetainSession` binding in v0.5.
- Linux and Windows integration tests cover the Issue #108 acceptance criteria, including crash
  points after each synchronized write in D4, using the ADR-0034 abrupt-process pattern.

## Consequences

* Good: retained durable buffers survive host restarts and can be discovered, read, and deleted
  explicitly, while compute and volatile state are never resurrected.
* Good: generation-scoped directories make stale cleanup unable to delete a recreated Session.
* Good: a corrupt or missing catalog can never cause silent deletion or partial trust.
* Bad: catalog mode moves directory ownership from the host factory into sitos and supports only
  RocksDB stores; hosts that need custom durable engines keep ADR-0032 mode without restarts.
* Bad: every durable lifecycle transition pays a synchronized catalog write.
* Bad: a single failed catalog write latches the whole process unavailable until restart.
* Bad: `Status` and the query wire gain two values and an error-reply form; older clients see
  `Error` for them.
* Neutral: retention remains a lifecycle record, not a seal; the host and trusted Worker contract
  still define complete output.
* Neutral: `deleted` tombstones accumulate until a sid is reused; no TTL is defined.

## Options Considered

* **Keep the host factory and store an opaque location in the catalog** — rejected because the
  catalog could not delete or reopen a store without a new host callback contract.
* **Per-Session JSON files with atomic rename** — rejected because directory fsync semantics
  differ on Windows, and RocksDB already provides synchronized atomic writes and exclusive locking.
* **Store directory first, record second** — rejected because a crash would leave an unknown
  directory that the node may never delete.
* **Disambiguate zero replies with a follow-up `meta/session` query instead of error replies** —
  rejected because the second query races lifecycle changes and still cannot type a degraded
  durable-buffer Get/List.
* **Fail `Start` when the catalog is unusable** — rejected because #108 requires the process to
  stay live with readiness false.
* **Latch only on open/validation failure, fail single writes softly** — rejected because RocksDB
  enters a background-error state after a failed synchronized write, and a later success would
  not prove the earlier state.

## References

* Issue #108; ADR prerequisite Issue #188, which records the owner's D1–D10 choices (2026-09-30)
* Related: ADR-0014, ADR-0017, ADR-0028, ADR-0032, ADR-0033, ADR-0034, ADR-0035
* Contract registry rows: session state-lost result; typed catalog-unavailable result; typed query
  error reply; catalog lifecycle state names
