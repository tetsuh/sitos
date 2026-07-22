# ADR-0028: Unify acknowledged operation results

## Status

Accepted — 2026-07-23

## Context

Acknowledged ParamStore writes (#14 and #17) and same-publisher fences (#106 and #107)
need the same correlation, result, polling, and duplicate-suppression machinery. The existing
wire outline records only successful token presence, while a StorageEngine write can fail without
proving whether the value took effect. PutBatch can also apply only a prefix. Defining these paths
independently would create incompatible acknowledgement formats and retry behavior. The contract
must preserve one data submission, bound all node-side state, distinguish an observed uncertain
outcome from an unobserved timeout, and work identically from C++ and Python.

## Decision

We will use one strict binary acknowledgement-result protocol for acknowledged Put, PutBatch, and
Fence operations. sitos will generate opaque UUIDv4 correlation tokens, submit data or a marker at
most once, retain bounded typed results in StorageNode, and retry only result queries within one
total deadline. `Status::OutcomeUnknown = 9` will represent a result observed by StorageNode for
which the current StorageEngine contract permits no stronger application claim.

### Token ownership and attachment

* The high-level C++ and Python APIs never accept caller-selected acknowledgement tokens.
* sitos generates a canonical random UUIDv4 for each acknowledged Put, PutBatch, or Fence. Tokens
  are correlation identifiers, not credentials, and are not restored after restart.
* The internal acknowledgement helper generates the token and passes it explicitly to the
  Transport adapter. The adapter encodes and decodes the token but never invents one.
* The canonical query spelling is lowercase `8-4-4-4-12` UUID text. The wire attachment carries
  the RFC 4122 16-byte UUID value rather than text.
* `AckAttachmentV1` is exactly 17 bytes:

  ```text
  offset  size  field
  0       1     schema_version = 1
  1       16    UUID bytes in RFC 4122 network order
  ```

* An absent attachment is an acknowledgement-free sample. An attachment with an unknown version,
  wrong length, or non-v4 UUID is rejected before application and recorded as a protocol error in
  operational diagnostics. No result is created when a valid query token cannot be recovered.
* Put and PutBatch have one token per wire message. Delete remains acknowledgement-free in v1.

### StorageNode token lifecycle

For each serialized application lane, StorageNode uses one bounded token state machine:

* The registry and completed-result ring belong to one live `StorageNode::State`, not the process.
  Every tokenized application lane shares that registry.
* `Processing` contains the token and an internal SHA-256 operation fingerprint over operation kind,
  full key, normalized Encoding identifier, and payload bytes. It is never evicted as a completed
  result. The digest is process-local collision detection, is not sent on the wire, is not a
  credential, and introduces no external dependency.
* `Completed` contains the same token and fingerprint plus its immutable `AckResultV1`.
* Before mutation, a callback atomically claims the token under the node-wide token-registry mutex;
  this claim is the token linearization point across all lanes. The mutex is released before engine
  application or synchronization and reacquired only to publish the immutable completion.
* Each serialized lane admits at most one `Processing` token. The parameter-write path is one lane;
  the fence ADR defines and bounds Publisher lanes. Reentrant admission is an invariant failure
  reported as `Status::Error` and does not apply the second operation.
* Completed results enter the node-local 4096-entry completion-order ring. Success, definite
  failure, and `OutcomeUnknown` consume entries equally. The ring is not LRU.
* A duplicate retained token with the same fingerprint never repeats apply or synchronization.
  While Processing it observes no query result; after completion every query receives the same
  immutable result.
* A retained token with a different fingerprint is a protocol collision. StorageNode rejects the
  new sample without application, preserves the original token state/result, and records a bounded
  sanitized diagnostic. A supported high-level caller cannot create this condition.
* Eviction removes both the completed result and fingerprint. After eviction, queries receive zero
  replies and StorageNode no longer promises duplicate suppression for that token. Clients must
  never use eviction or timeout as a reason to resubmit data.
* An exception or unexpected internal failure after claim is contained at the callback boundary,
  and an RAII completion guard transitions the token exactly once to an immutable `Completed`
  result. Before any StorageEngine mutation or synchronization call is invoked, such a failure is
  `Status::Error`. Once a mutation or synchronization call has been invoked, an exception is
  `Status::OutcomeUnknown` unless the callee's contract proves the requested effect did not occur.
  A normally returned result retains its ordinary success or failure semantics; later bookkeeping
  must not weaken a result whose effect is already proven. For a batch, the completion preserves
  the confirmed prefix and identifies the entry whose engine invocation threw. No token-registry
  lock is held while calling a StorageEngine, Transport callback, or synchronization primitive.
* StorageNode Stop first rejects new callbacks, quiesces admitted callbacks under ADR-0017, then
  clears Processing and Completed state. A later Start does not recover old results.

### AckResult v1 wire schema

The transport-independent Encoding identifier is `sitos.v1.ack`; the canonical Zenoh Encoding is
`zenoh/bytes;sitos.v1.ack`. `AckResultV1` has this exact little-endian layout:

```text
offset  size  field
0       1     schema_version = 1
1       1     operation_kind: put = 1, batch = 2, fence = 3
2       1     status: stable sitos Status numeric value
3       1     durability: applied = 1, synced = 2
4       4     applied_count_le
8       4     failed_index_le; UINT32_MAX means none/not applicable
12      8     through_sequence_le; 0 means not applicable
20      8     failed_sequence_le; UINT64_MAX means none/not applicable
28      4     message_length_le
32      n     sanitized UTF-8 message; 0 <= n <= 1024
```

The encoded length must equal `32 + message_length`. Unknown versions, operation kinds,
durability values, or Status values; invalid sentinels; invalid UTF-8; and truncated, trailing, or
overlong data are protocol errors. AckResult v1 has this closed Status allowlist for every operation:
`Ok = 0`, `NotFound = 1`, `TypeMismatch = 2`, `Disconnected = 4`, `ReadOnly = 5`,
`InvalidKey = 6`, `InvalidArgument = 7`, `Error = 8`, and `OutcomeUnknown = 9`.
`Timeout = 3` is client-only and is rejected on the wire. A future Status is also rejected by v1
unless a later Accepted ADR explicitly adds it without changing existing meanings; merely appending
the public Status enum does not expand this wire allowlist. The `durability` field is the requested
target: `Ok` confirms that target, while a non-OK result says it was not confirmed.

Operation-specific invariants are:

| Operation/result | `durability` | `applied_count` | `failed_index` | sequence fields |
|---|---|---:|---:|---|
| Put success | applied | 1 | none | not applicable |
| Put failure/unknown | applied | 0 | 0 | not applicable |
| Batch success | applied | entry count | none | not applicable |
| Batch envelope/global validation failure | applied | 0 | none | not applicable |
| Batch entry validation failure | applied | 0 | first invalid entry | not applicable |
| Batch application failure/unknown | applied | confirmed prefix count | first failed entry | not applicable |
| Fence | applied or synced | 0 | none | `through_sequence`; optional first `failed_sequence` |

For a batch envelope or other whole-message validation failure that cannot be attributed to one
entry, `failed_index` is `UINT32_MAX`. If a fully decoded entry fails validation, `failed_index`
names that entry even though complete prevalidation keeps `applied_count` at zero. During engine
application, `failed_index` names the first entry that returned failure or threw, and
`applied_count` contains only the preceding confirmed prefix.

Fence sequence zero represents an empty covered prefix. A non-sentinel `failed_sequence` must be
nonzero and no greater than `through_sequence`.

Remote results contain only a portable Status and a sanitized bounded UTF-8 message. Native
platform error causes remain in local `Result` diagnostics and never cross the wire. The result
message is informative and does not change Status semantics.

`<prefix>/meta/ack/<token>` accepts the existing safe route grammar, but only sitos-generated
canonical UUIDv4 tokens can name results. The deployment has one authoritative StorageNode for an
ACK namespace. Under ADR-0020, the exact-key Get surface exposes at most one consolidated reply;
this ADR does not claim that the client can count native Zenoh replies. The observable reply must
use the exact query key and Encoding `sitos.v1.ack`. Absent, Processing, evicted, and restart-lost
tokens return zero replies. A wrong reply key/Encoding or malformed result is a protocol error.

### Application and batch outcome

StorageNode decodes and validates the complete operation before its first engine mutation. With a
valid token, a definite validation rejection creates a completed typed failure result without
mutating storage. A batch envelope or whole-message rejection uses `applied_count = 0` and
`failed_index = UINT32_MAX`; an entry-specific rejection uses `applied_count = 0` and the invalid
entry's index.

PutBatch remains one canonical wire message and is not transactional:

1. validate every entry;
2. apply entries in caller order;
3. stop at the first `StorageEngine::Put` failure;
4. report only the confirmed prefix in `applied_count` and the first failed entry in
   `failed_index`;
5. do not attempt later entries.

The current boolean StorageEngine write result cannot prove whether a `false` operation had no
effect. StorageNode therefore reports `OutcomeUnknown` for that entry. The same conservative result
applies when `StorageEngine::Put` throws after invocation because its current contract cannot prove
whether mutation occurred. Failures before invoking the engine are definite `Error` or validation
statuses. A future typed, exception-safe engine result may tighten a known rejection to a definite
non-OK Status without changing AckResult v1.

The same stop-first application rule applies to acknowledged and acknowledgement-free batches.
ParamCaches still receive and locally apply the complete pub/sub batch, so entries at or after the
StorageNode failure can disagree with StorageNode-derived snapshots and SessionView. ACK does not
repair this failure-path asymmetry; #20 owns future authoritative re-fetch.

### Submission and total-deadline policy

`WriteOptions` is:

```cpp
struct WriteOptions {
  bool ack = true;
  std::chrono::milliseconds ack_timeout{3000};
};
```

Python ParamStore exposes keyword-only `ack=True` and `ack_timeout_ms=3000`. Acknowledgement-free
writes return after Transport submission. Acknowledged writes require a positive timeout. Empty
PutBatch returns immediate `Ok` with no data submission, token, or query.

The total deadline starts immediately before the sole data Put/PutBatch call, after local input and
option validation. `PutOptions` carries the helper-generated explicit token instead of asking the
adapter to generate one. Existing Transport implementations and mocks must forward that token.
Because the current `Result<void> Put(...)` contract has no reliable post-call submission phase,
every non-OK return after invoking Put is conservatively `MayHaveSubmitted`; it does not cause a
second Put. Definite `NotSubmitted` results are limited to validation performed before invoking
Transport. This rule avoids a new public native-submission-disposition API.

| Observation | Data submission | ACK behavior | High-level result |
|---|---:|---|---|
| Local validation failure | none | none | definite validation Status |
| Put returns `Ok` | exactly once | query immediately, then within deadline | decoded AckResult Status |
| Put returns non-OK | at most one possible | continue querying within deadline | AckResult if observed; otherwise `Timeout` |
| Zero reply or Transport Get failure | no resubmission | wait at least 100 ms, then query again | `Timeout` only when total deadline expires |
| Malformed key/Encoding/result | no resubmission | stop querying | `Error` |
| StorageNode boolean engine failure | exactly once | return cached result | `OutcomeUnknown` |

Each query window is `min(1000 ms, remaining deadline)`, only one query is active at a time, and
there is no attempt-count limit in addition to the total deadline. If submission consumes the whole
deadline, the client returns `Timeout` without starting a non-positive query window. Query failures
retain their latest native cause for local diagnostics but do not justify data resubmission. After
Get quiesces, a protocol error takes precedence; otherwise one valid decoded AckResult takes
precedence over a non-OK Get completion; otherwise the Get failure or zero reply is retried. A late
result does not revive a completed client call.

`Timeout` means no valid AckResult was observed by the deadline; none, some, or all effects may have
occurred. `OutcomeUnknown` means StorageNode did observe and attempt the operation but could make no
stronger application claim. Neither status causes automatic retry or reconnection.

`Status::OutcomeUnknown = 9` is appended without renumbering existing Status values and maps to
`OutcomeUnknownError(SitosError)` in Python.

ParamCache Put and PutBatch remain submission-first and initiating-cache local-apply. They do not
receive these acknowledgement options. ParamStore Delete also remains acknowledgement-free in v1.

### Fence reuse boundary

The shared UUID generator, attachment codec, AckResult codec, completed-result ring, exact ACK
query route, and total-deadline query helper are also used by Fence. A second Fence result protocol
is prohibited.

The same-publisher marker layout, ordering lane, per-Publisher failure aggregation, durable and
ephemeral publisher behavior, and ParamCache local-delivery waiter lifecycle are defined by
ADR-0029 under #106 ownership. ADR-0029 must reuse this result protocol, and both ADRs must be
Accepted before #99 or #107 implementation begins. Synced Fence additionally remains blocked on
#105's accepted StorageEngine durability contract.

### Qualification requirements

Implementation must include:

* golden-byte and negative codec tests for the attachment and result, including every enum,
  sentinel, length, UTF-8, key, and Encoding rule;
* deterministic tests proving one data submission, retained-token non-reapplication, collision
  rejection, completion-ring eviction, shutdown clearing, and no callback after return;
* tests for every submission/deadline matrix row and late-result behavior;
* PutBatch confirmed-prefix tests and explicit ParamCache/StorageNode divergence tests;
* real Zenoh Linux and Windows attachment/query round trips, sanitizer coverage, and bounded-state
  qualification including the maximum 1024-byte result message;
* C++/Python parity tests for options, timeout behavior, and `OutcomeUnknownError`.

## Consequences

* Good: ParamStore writes and fences share one versioned result format and one polling policy.
* Good: callers can distinguish definite rejection, observed uncertainty, and unobserved timeout.
* Good: bounded Processing and Completed state prevents unbounded token accumulation.
* Good: one-submit semantics prevent acknowledgement retry from repeating subscriber-visible effects.
* Bad: ParamStore writes become acknowledged by default and gain up to the configured deadline in
  latency; callers must choose `ack = false` for submission-only behavior.
* Bad: result eviction intentionally limits duplicate suppression and can turn a successfully
  applied operation into a client-visible Timeout.
* Bad: SHA-256 fingerprinting adds bounded CPU work for acknowledged parameter writes.
* Neutral: PutBatch remains non-transactional and its cache/node failure-path asymmetry remains.
* Neutral: incompatible peers are rejected; v1 has no version negotiation or fallback.
* Neutral: Fence ordering semantics remain in a separate ADR while reusing this wire result.

## Options Considered

* **Keep a positive token-only ring** — rejected because it cannot report definite failure,
  partial batch application, or an observed uncertain engine outcome.
* **Use separate write and Fence ACK formats** — rejected because correlation, result caching,
  deadline polling, and duplicate suppression are the same mechanism.
* **Resubmit data after timeout** — rejected because pub/sub effects can be repeated and the server
  may have applied a result that was evicted or whose reply was lost.
* **Expose client-selected tokens and retry tuning** — rejected because it expands the public API,
  permits accidental token reuse, and makes retry behavior inconsistent.
* **Make PutBatch transactional** — rejected because current engines and readers provide no
  multi-key transaction or same-snapshot read contract.
* **Persist ACK state across restart** — rejected because acknowledgements are bounded live
  correlation state, not an execution journal.

## References

* Proposal and design authority: Issue #114
* Implementation issues: #14 and #17
* Fence consumers: #99, #106, and #107
* Resynchronization boundary: #20
* Related: ADR-0017 (StorageNode lifecycle), ADR-0019 (Result/Status), ADR-0020
  (synchronous Transport Get), ADR-0023 (ParamCache consistency and lifetime)
* Specifications to update after acceptance: `docs/02_architecture.md` §§4.4 and 7.2,
  `docs/03_wire_protocol.md` §6, `docs/04_api_cpp.md`, and `docs/05_api_python.md`
