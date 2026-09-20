# ADR-0034: Define explicit storage durability barriers

## Status

Proposed — 2026-09-20

## Context

`StorageEngine::Put` currently returns `bool` after engine application but defines no stable-storage
boundary. Durable BufferPublisher fences need one explicit barrier after a covered prefix and before
an application treats a committed manifest as complete. ADR-0029 already owns Fence ordering and
ADR-0028 already owns remote result and ambiguity semantics, so this ADR must define only the engine
capability and synchronization boundary.

A successful no-op is useful for `InMemoryEngine` contract uniformity but cannot mean power-loss
durability. Third-party engines must not silently inherit such a claim. RocksDB writes must retain
WAL batching (`WriteOptions::sync == false`) while one explicit barrier flushes and synchronizes the
WAL. Orderly close/reopen and process-kill tests alone are insufficient proof of an fsync-class call.

## Decision

We will add an additive `StorageEngine::Sync()` result operation and an explicit synchronization
capability:

```cpp
enum class SyncCapability {
  kUnsupported = 0,
  kVolatileNoop = 1,
  kPowerLossDurable = 2,
};

class StorageEngine : public StorageReader {
 public:
  virtual SyncCapability GetSyncCapability() const noexcept;
  virtual Result<void> Sync();
};
```

The numeric values are stable and append-only: existing values will never be renumbered or reused,
and unknown values are invalid. The default implementations report `kUnsupported` and return
`Status::Error` with `std::errc::operation_not_supported`. These non-pure virtual defaults preserve
source compatibility for custom derived engines while making non-support explicit. Adding the
virtuals intentionally changes the public C++ ABI, so every consumer must rebuild against the
resulting sitos library. A custom engine may report `kPowerLossDurable` only when a successful
`Sync()` guarantees that every covered mutation survives power loss under its documented
platform/storage assumptions.

### Ordering and linearization

Each engine will make `Put`, `Delete`, and `Sync` linearizable in one engine-local mutation order.
A `Sync()` linearization point lies between its invocation and completion. A successful return
covers every successful mutation ordered before that point; a mutation ordered after that point is
not promised, even if the backend happens to include it. A completed mutation that precedes Sync
invocation must therefore be covered, while an overlapping mutation may be ordered on either side.

This is an observable contract, not a mandated new mutex. `InMemoryEngine::mutex_` is an existing
production mutation lock and can be reused. RocksDB `DatabaseState::operations_mutex`, however,
exists only under `SITOS_ROCKSDB_TEST_SEAM`; it protects `OperationAdapter` state, and the adapter
operation is invoked after that lock is released. It is neither production sequencing nor a
reusable durability lock. RocksDB must therefore prove an equivalent backend-native order or add
the minimum production synchronization. Reads and snapshots retain their existing concurrency
contract and need not join the mutation order unless the implementation already requires it.

No implicit barrier is added to `Put` or `Delete`. The barrier may synchronize unrelated writes to
the same engine; it does not create a transaction, snapshot, seal, authorization boundary, or
exactly-once guarantee.

### Engine behavior

- `InMemoryEngine` reports `kVolatileNoop`. Its `Sync()` joins the implementation's existing
  mutation order and returns success, proving only the ordering of in-process applied state. It
  never qualifies a durable BufferPublisher `kSynced` receipt.
- `RocksDBEngine` reports `kPowerLossDurable`. It explicitly keeps WAL enabled, keeps per-write
  synchronization disabled, joins the engine mutation order, and calls RocksDB 11.1.2
  `DB::FlushWAL(true)`. This covers RocksDB's optional internal WAL buffer and then performs
  `SyncWAL`; `SyncWAL()` alone is not the implementation.
- A third-party engine inherits unsupported behavior unless it overrides both operations. Returning
  success without advertising `kPowerLossDurable` cannot enable a durable synchronized Fence.

Downstream #107 integration will admit a synchronized durable-buffer marker only for a Session
engine advertising `kPowerLossDurable`. A volatile or unsupported engine produces ADR-0029's
definite disabled-capability failure and must not invoke `Sync()`. Applied fences remain available.
Ephemeral synchronized fences remain invalid under ADR-0029. These StorageNode/BufferPublisher tests
belong to #107 and consume this contract; they are not #105 engine acceptance criteria.

### Failure semantics

At the engine API, a failure before invoking the backend synchronization primitive is a definite
`Status::Error`. Once backend synchronization is invoked, this ADR newly chooses
`Status::OutcomeUnknown` for a non-OK return or exception unless the backend contract proves that
none of the requested durability effect occurred. The reason is local and semantic: after native
progress, the failed barrier cannot certify the requested durable prefix. This choice is
conservative; ADR-0029 permits an #105-defined allowlisted Status but does not itself mandate
`OutcomeUnknown` for every native failure. A standalone Sync may be safely retryable; no automatic
retry is a policy of this API/Fence composition, not a claim that Sync is non-idempotent. Native
causes and bounded diagnostics are preserved.

Downstream #107 mapping consumes this engine Result: `Error` and `OutcomeUnknown` already belong to
ADR-0028's closed allowlist, so no Status value or wire format is added. ADR-0029 remains
authoritative for the Fence receipt, failed-sequence sentinel, timeout, and Publisher usability
after an unrecoverable Fence result.

### Lifetime and shutdown

Calling an engine method concurrently with destruction is outside the C++ object-lifetime contract.
The engine owner must quiesce calls before destruction, and any engine ordering primitive is
released before returning the `Result`. Downstream #107 integration must prove that StorageNode's
existing callback, Session-admission, and quiescence gates meet that precondition and publish the
immutable marker result before destroying the Session engine; that proof is not a #105 engine test.

RocksDB snapshots retain ADR-0033's shared database ownership. `Sync()` uses the live engine instance
and does not change snapshot semantics or authorize filesystem deletion.

### Qualification

Before implementation, the public capability enum will be registered as a Planned stable-identifier
row owned by this Proposed ADR, as required by the Contract Registry. Issue #105 requires only the
engine-contract evidence below:

1. reusable contract tests for unsupported custom engines, InMemory successful volatile no-op, and
   precise pre-/post-linearization mutation coverage;
2. a RocksDB native seam proving the exact `FlushWAL(true)` call and injected non-OK/throw mapping;
3. proof that ordinary writes use WAL but do not set per-write sync;
4. process-isolated Linux and Windows tests that hard-stop the writer after a successful barrier,
   without running engine/node destructors, then reopen and verify exact values; and
5. documentation that process termination leaves the OS page cache and is not a complete physical
   power-loss emulator; the power-loss claim is bounded by the RocksDB, filesystem, OS, and hardware
   synchronization contracts.

An orderly close/reopen test may supplement but cannot satisfy the crash oracle by itself.
StorageNode synced-Fence admission and remote result mapping are #107 acceptance criteria. #108
ADR/design may proceed alongside #105, and its implementation follows its own readiness and
accepted dependencies. The combined manifest/retain/restart/delete qualification waits for both
#107 and #108; its catalog crash points remain #108 integration criteria and do not expand #105
scope.

## Consequences

- Good: BufferPublisher can distinguish receiver application from a real persistent synchronization
  capability without changing every ordinary write.
- Good: InMemory keeps a successful uniform barrier for ordering tests without falsely claiming disk
  or power-loss persistence.
- Good: custom engines fail safely by default and opt in only with an explicit guarantee.
- Good: the RocksDB operation includes manual WAL buffers and preserves batched, unsynchronized
  ordinary writes.
- Good: ambiguous synchronization failures reuse `OutcomeUnknown`; no second result protocol or
  Status value is introduced.
- Bad: proving one mutation order may require additional synchronization and can reduce concurrent
  write throughput around a barrier when existing/native ordering is insufficient.
- Bad: adding public virtual methods changes the C++ ABI and requires consumers to rebuild.
- Bad: CI can prove the native sync call and abrupt-process recovery but cannot fully emulate every
  filesystem, firmware, or power-loss failure mode.
- Neutral: a barrier can synchronize more backend writes than its minimum covered prefix; callers
  may rely only on the specified prefix.
- Neutral: this ADR does not define BufferPublisher API shape, catalog storage/layout, retention,
  deletion, Python engine plugins, or Python catalog hosting.

## Options Considered

- **Only `Result<void> Sync()` with no capability query** — rejected because successful InMemory
  no-op and power-loss durability would be indistinguishable to StorageNode.
- **Pure-virtual Sync/capability** — rejected because every existing custom engine would be forced to
  implement a durability claim or boilerplate; safe default unsupported behavior is smaller.
- **Successful default no-op** — rejected because a third-party engine could accidentally enable a
  false synchronized receipt.
- **Synchronize every Put** — rejected because it removes the explicit batching boundary and violates
  Issue #105's no-per-Put-sync requirement.
- **Use `DB::SyncWAL()` only** — rejected because it does not flush RocksDB's internal WAL buffer when
  `manual_wal_flush` is enabled.
- **Flush memtables/SST files on every barrier** — rejected because WAL-backed recovery supplies the
  required boundary with less amplification; this ADR does not require compaction or an SST seal.
- **Treat every native sync error as definite non-durability** — rejected because partial progress
  can make the durable outcome unknowable.
- **Add a Python RocksDB or catalog backend** — rejected because the product host is C++; Python
  parity is required for #107 Workers, while Python hosting remains simple prototyping.

## References

- Proposal: [Issue #181](https://github.com/tetsuh/sitos/issues/181)
- Implementation owner: [Issue #105](https://github.com/tetsuh/sitos/issues/105)
- Consumer: [Issue #107](https://github.com/tetsuh/sitos/issues/107)
- Restart/catalog owner: [Issue #108](https://github.com/tetsuh/sitos/issues/108)
- ADR-0028: acknowledged-operation results and `OutcomeUnknown`
- ADR-0029: same-publisher Fence ordering and synchronized-marker composition
- ADR-0032: mixed durable/ephemeral Session buffer routes
- ADR-0033: RocksDB engine, snapshot, package, and cleanup boundary
- RocksDB v11.1.2 [`DB::FlushWAL` and `DB::SyncWAL`](https://github.com/facebook/rocksdb/blob/v11.1.2/include/rocksdb/db.h)
- RocksDB v11.1.2 [`WriteOptions` and `manual_wal_flush`](https://github.com/facebook/rocksdb/blob/v11.1.2/include/rocksdb/options.h)
