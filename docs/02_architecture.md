# sitos — Architecture Specification

Requirement IDs ([01_requirements.md](01_requirements.md)) are referenced as [F..]/[N..].

## 1. Component Structure

```
 ┌─ Host process (e.g., controller / orchestrator) ──────────┐
 │                                                            │
 │  zenoh session (peer)                                      │
 │   ├ queryable("<prefix>/**")   ◄─── zenoh get ─────────────┼───┐
 │   ├ subscriber("<prefix>/**")  ◄─── zenoh put/delete ──────┼───┤
 │   │            │                                           │   │
 │   │   ┌────────▼─────────┐   ┌───────────────────────┐     │   │
 │   │   │   StorageNode    │──►│ StorageEngine (abs.)  │     │   │
 │   │   │  - base R/W      │   │  InMemoryEngine       │     │   │
 │   │   │  - overlay mgmt  │   │  RocksDBEngine        │     │   │
 │   │   │  - snapshot mgmt │   │  (user-defined...)    │     │   │
 │   │   └──────────────────┘   └───────────────────────┘     │   │
 │   │                                                        │   │
 │  ParamStore (write / List API)                             │   │
 │  session lifecycle (StorageNode API)                       │   │
 └────────────────────────────────────────────────────────────┘   │
                                                                   │
 ┌─ Subscriber process (e.g., compute worker) ×N ───────────┐      │
 │  zenoh session (peer) ───────────────────────────────────┼──────┘
 │   ├ get (initial fetch)                                  │
 │   └ subscriber("<prefix>/session/<id>/**") (delta)        │
 │            │                                             │
 │   ┌────────▼────────┐                                    │
 │   │   ParamCache    │ ← zero-copy compute reads           │
 │   └─────────────────┘                                    │
 └──────────────────────────────────────────────────────────┘
```

| Component | Role | Dependencies |
|---|---|---|
| `StorageEngine` | Persistence abstraction. put/get/list/delete + optional snapshot | None (zenoh-independent) [X01] |
| `Transport` | Thin adapter that hides the zenoh API. Limited to put/get/queryable/subscriber/delete/attachment/encoding functionality | zenoh |
| `StorageNode` | Zenoh queryable/subscriber ↔ engine; owns Session lifecycles | zenoh, StorageEngine |
| `ParamStore` | Client API: typed Put/Get/List/Delete/Subscribe. Wraps a zenoh session | zenoh |
| `ParamCache` | Subscriber-side read cache. Initial fetch + delta subscription + zero-copy Get | zenoh |
| Session lifecycle | Conceptual responsibility inside the StorageNode-owning process; use `StorageNode::CreateSession`, `CloseSession`, and `ActiveSessions` | StorageNode |
| `SessionView` | Host-process read-only view that resolves session overlay → snapshot | StorageNode |

**Design principle**: `engine/` does not know about zenoh. `ParamStore`/`ParamCache` do not
know about engine. Direct dependencies on the zenoh-c API are confined to the
`transport/zenoh` layer, and higher-level components see only the `Transport` abstraction.
This limits the impact scope when zenoh is upgraded ([09_dependency_policy.md](09_dependency_policy.md)).

## 2. Key Space

Default prefix: `sitos` (configurable [X03]). For the detailed grammar and normalization rules,
see [03_wire_protocol.md](03_wire_protocol.md).

```
<prefix>/base/<key...>                 # master data
<prefix>/session/<sid>/<key...>        # session overlay (Put during execution)
<prefix>/snap/<sid>/<key...>           # read view of the session snapshot (read-only)
<prefix>/buffers/<sid>/durable/<key...>   # stored values; Zenoh fanout is independent [ADR-0032]
<prefix>/buffers/<sid>/ephemeral/<key...> # live-only values; no node retention [ADR-0032]
<prefix>/meta/session/<sid>            # session metadata (state, creation time)
```

* put to `base/**` → the StorageNode subscriber writes to the engine [F04]
* get to `base/**` → the StorageNode queryable responds from the engine [F03, F08]
* raw DELETE to `base/**` or `session/<sid>/**` → remove the selected value. The public
  ParamStore Delete API remains base-only; snapshots are read-only and buffer DELETE is
  unsupported in v0.4
* put to `session/<sid>/**` → StorageNode records it in the overlay (an in-memory map
  separate from the engine). zenoh directly distributes it to subscribers (ParamCache) [F06]
* get to `snap/<sid>/**` → StorageNode responds from the snapshot view.
  put/delete through the library API returns a `ReadOnly` error; raw zenoh put/delete is
  fire-and-forget, so it is ignored + a warning is logged [F05, F08]
* put to `buffers/<sid>/durable/**` → if the Session enables durable buffers, StorageNode admits
  the plain bytes under the write-once rule and stores them in its one durable buffer engine;
  Zenoh fanout to other subscribers is independent [ADR-0032]
* put to `buffers/<sid>/ephemeral/**` → if enabled, StorageNode admits and processes the plain
  bytes without storing them; Zenoh fanout is independent and StorageNode cannot retract another
  subscriber's observation [ADR-0032]
* get/list to `buffers/<sid>/durable/**` → the queryable responds from the Session's durable
  engine. Ephemeral routes have no Get or replay semantics [ADR-0032]
* Buffer routes are disjoint from ParamStore, ParamCache, ParamSubscription, and SessionView.
  They use no sitos payload schema or type tag, and support no `:batch`, `:fence`, snapshot, or
  other control namespace in v0.4 [ADR-0032]
* Durable values live until `CloseSession`. CloseSession destroys engine ownership before
  returning; physical directory removal is host-owned afterward. A new v0.4 Session receives a
  fresh or logically empty store. Restart catalogs and deletion retry belong to #108 [ADR-0032]

## 3. StorageEngine Abstraction

```cpp
namespace sitos {

using Bytes     = std::span<const std::byte>;
using EntrySink = std::function<bool(std::string_view key, Bytes value)>;

/// Read-only view. Common type for the engine itself and snapshots.
class StorageReader {
public:
    virtual ~StorageReader() = default;
    /// If key exists, call sink once and return true. If not, return false.
    virtual bool Get(std::string_view key, const EntrySink& sink) const = 0;
    /// Call sink for every entry that matches prefix (by prefix match on the key string).
    /// If sink returns false, abort and return false. If iteration completes, return true.
    virtual bool List(std::string_view prefix, const EntrySink& sink) const = 0;
};

class StorageEngine : public StorageReader {
public:
    virtual bool Put(std::string_view key, Bytes value) = 0;
    virtual bool Delete(std::string_view key)           = 0;

    /// Return a consistent read view at that point in time.
    /// Default implementation: an InMemory view copied in full via List (O(n)) [N03].
    /// LevelDB/RocksDB-style engines implement this as O(1) with native snapshots [N02].
    virtual std::shared_ptr<const StorageReader> TakeSnapshot() const;
};

} // namespace sitos
```

Conventions:

* Engines treat values as opaque byte sequences (they do not know the payload format)
* Pointers passed to `Get`/`List` sinks are valid only during the sink call
* Thread safety: the engine guarantees safe concurrent reads + safe concurrent read/write [N07].
  Sinks run without an internal engine lock that prevents reentrant engine calls; their key and
  value views remain valid for the sink call, and `List` enumerates a consistent read set.
  `InMemoryEngine` uses `std::shared_mutex`; RocksDB uses its native guarantees
* The view returned by `TakeSnapshot()` is not affected by Put/Delete operations after the call

## 4. StorageNode

### 4.1 Responsibilities

1. Declare zenoh `queryable`: respond to get requests for `<prefix>/**`
   - `base/**` → engine
   - `snap/<sid>/**` → view in the snapshot table
   - `session/<sid>/**` → overlay table
   - `buffers/<sid>/durable/**` → the Session's durable buffer engine
   - `buffers/<sid>/ephemeral/**` → no replies; ephemeral is live-only [ADR-0032]
2. Declare zenoh `subscriber`: receive put/delete for `<prefix>/**`
   - `base/**` → apply to engine
   - `session/<sid>/**` → apply to overlay
   - put to `snap/**` → ignore + warning log (read-only)
   - buffer PUTs → capability admission and durable write-once handling; Zenoh fanout is
     independent [ADR-0032]
   - buffer DELETEs and control routes → reject as unsupported in v0.4
3. Session lifecycle (a conceptual responsibility implemented inside StorageNode):
   - `StorageNode::CreateSession(sid, options)`: reserve a `Creating` Session record, create the
     snapshot, overlay, metadata, capabilities, and one durable engine through the host factory
     only when enabled; roll back every resource on failure, then transition to `Active`
   - `StorageNode::CloseSession(sid)`: transition the record to `Closing`, close its admission
     gate, wait for admitted callbacks or operations, destroy its durable engine, and release all
     other resources before returning [F10, ADR-0032]
   - `StorageNode::ActiveSessions()`: return the active Session ids in unspecified order; return
     an empty list when the node is stopped.

Earlier diagrams and design discussions may call this responsibility `SessionController`. It is a
conceptual responsibility inside the process that owns a `StorageNode`, not a public constructible
C++ type. Applications use the `StorageNode` session methods above.

### 4.2 Data Structures

Stage #141 implements the mixed buffer routes with `SessionOptions` and a
node-owned `DurableBufferEngineFactory`. Durable PUT admission accepts only
`zenoh/bytes`, performs the engine read/compare/write sequence under the
subscriber mutex, and retains no failed-key reservation. Ephemeral PUTs are
capability-checked and never enter node storage. Durable query results are
materialized before releasing Session admission; engine collection failures
produce zero replies, while reply-handler failures after dispatch suppress
subsequent replies without promising atomic transport replies.

```cpp
enum class SessionPhase { Creating, Active, Closing };

struct SessionRecord {
    SessionPhase phase;
    std::shared_ptr<const StorageReader> snapshot;
    std::shared_ptr<StorageEngine> overlay;
    std::unique_ptr<StorageEngine> durable_buffers;  // null unless enabled
    SessionOptions options;
    SessionMeta metadata;
    SessionAdmission admission;
};

// Lives inside StorageNode's callback-shared long-lived State (see §4.4 / ADR-0017).
struct State /* excerpt */ {
    std::string prefix;  // validated key prefix used by callbacks
    std::shared_ptr<StorageEngine> engine;
    DurableBufferEngineFactory durable_buffer_engine_factory;
    std::unordered_map<std::string, std::shared_ptr<SessionRecord>> sessions;
    // Serializes the complete subscriber application path, including batch entries.
    std::mutex subscriber_mutex;
    // The node-wide callback gate remains responsible for Stop() quiescence.
    CallbackGate callback_gate;
    std::shared_mutex session_mutex;
};
```

Every callback or operation that can retain or use a Session snapshot, overlay, metadata, or buffer
engine takes that record's admission lease while it uses the resource. The lock order is node gate
→ `subscriber_mutex` (subscriber only) → `session_mutex` → Session admission. Both Transport
callbacks acquire the node-wide callback-gate lease before parsing or inspecting a sample/query. A
subscriber then holds `subscriber_mutex` across its complete parse and application path,
including a durable `ReadExact` followed by the write-once decision and `Put`; it releases
`session_mutex` before any engine operation or log emission. Query callbacks never take
`subscriber_mutex`: they use the gate, then `AcquireActiveAdmission`, which atomically finds an
`Active` record and acquires its admission under `session_mutex`, and returns only after releasing
`session_mutex`.

`AcquireActiveAdmission(sid)` takes a shared `session_mutex`, finds the SID, and checks that its
phase is `Active`. It attempts the record's admission lease before returning either an empty result
or the record plus lease. No caller may use the record without the lease, and the helper never
performs engine work or logging while `session_mutex` is held. Closing a record prevents new
leases, waits admitted callbacks and operations, then destroys its durable engine and releases
Session resources. Readers copy ownership only while the record is active, so close cannot
invalidate an in-flight reply. The node-wide callback gate still makes `Stop()` quiescent.

The phase contract is exact: `absent → Creating → Active` for creation, and
`Active → Closing → absent` for close. If the node State is absent, or a callback has captured a
State whose lifecycle gate is closed before admission, both `CreateSession(std::string_view)`
overloads return `Status::InvalidArgument` with cause `std::errc::invalid_argument` and an empty
message, as required by DEC-140-STOP-STATUS-002. This method-specific exception is distinct from
generic disconnected statuses documented for other APIs. `CreateSession` reserves a non-queryable
`Creating` record before external factory creation. Creation against `Creating` or `Closing` returns
`std::errc::operation_in_progress`, while a duplicate `Active` creation retains
`std::errc::file_exists`. The creator commits only after taking `session_mutex` and verifying
that the same reservation still exists and remains `Creating`; it then changes that record to
`Active`. The reservation lock is released before external factory or engine operations and
logging. Independent `CreateSession` calls may execute concurrently and may invoke the stored
factory concurrently; a factory implementation with mutable shared state is responsible for
synchronizing that state. `CreateSession` uses the callback-shared State generation captured by the
node, so its factory and resources cannot come from a different Start generation. The factory is
moved from `StorageNodeConfig` into that State before Transport declarations.

Only a valid, non-null durable engine may commit `Creating` to `Active`. For a
Session requesting durable buffers, a factory `Err` is returned with its status, cause, and
message preserved; an empty factory, `Ok(nullptr)`, or a factory exception is a non-OK
failure. DEC-56-FACTORY-FAILURE-001 defines the exact taxonomy: a missing factory is
`Status::InvalidArgument` with `std::errc::invalid_argument` and the message `durable buffer
engine factory is required`; null success and exceptions are `Status::Error` with their exact
public messages; and factory `Err` values are preserved. Exceptions are contained. Every
non-commit path releases all resources created by that attempt and removes only the same
`Creating` reservation under `session_mutex`, so the same SID can be retried.

`CloseSession` changes only an `Active` record to `Closing`; a `Creating` or `Closing` record
returns `std::errc::operation_in_progress`, and a missing record retains
`std::errc::no_such_file_or_directory`. The close operation releases `session_mutex` before
callback quiescence, destruction, external engine work, or logging. Same-SID lifecycle phase
collisions do not wait and return `std::errc::operation_in_progress`; admission quiescence is a
separate required wait and may use the gate's normal synchronization implementation. This
contract does not prescribe condition-variable internals. Close leaves the SID reserved until
quiescence and all resource release complete. `CreateSession`, `CloseSession`, and
`ActiveSessions` also enroll in the node callback gate before using lifecycle state, so
`Stop()` remains a node-wide quiescence boundary.

`ActiveSessions` takes the node callback-gate lease, then takes `session_mutex` and copies only
SIDs whose records are `SessionPhase::Active`. It releases `session_mutex` and the gate lease
before returning and retains no Session record or other resource after enumeration. `Creating` and
`Closing` SIDs are not externally listed.

### 4.3 Consistency Model

> **Normative design; implementation planned:** Accepted ADR-0029 owns the same-publisher Fence
> mechanism below; #158 owns production implementation.

* Same-publisher Fence ordering is not inferred from a Zenoh session alone. ADR-0029 defines a
  sitos logical Publisher as a serialized UUIDv4-and-sequence lane whose covered data and marker
  use one Fence-capable Transport generation, reliable delivery, `Block` congestion control,
  identical `Data` priority, and non-express submission. Multiple logical Publishers may share a
  Transport or session, but their lanes remain isolated only under ADR-0029's collision-resistant
  generated-UUID non-collision condition; no cross-Publisher order is promised. A detected Transport
  generation change permanently disconnects the existing Publisher; resumption requires a new
  Publisher UUID and sequence lane. Unsupported or uninspectable QoS/topology is rejected rather
  than weakening the guarantee.
* `FenceLaneAttachmentV1` lets the designated receiver prove a contiguous covered sequence.
  Marker arrival alone cannot confirm a missing or reordered publication. The Transport's bounded
  FIFO callback-dispatch lane also prevents a marker entered after a data callback from completing
  before that callback's processing returns. Production implementation and executable
  qualification remain planned under #158.
* The local-delivery receiver boundary is completion of the initiating ParamCache Attach
  generation's serialized decode/cache-mutation path; it is not peer delivery or StorageNode
  acknowledgement. A buffer marker binds both SID and one generated active Session-generation UUID,
  so under the receiver-generation UUID non-collision condition same-SID recreation cannot answer an
  old marker. StorageNode rejects a mismatched generation
  before ACK-token claim; delayed data isolation across recreation remains outside the Fence
  guarantee. Successful atomic active-Session admission is the marker/CloseSession linearization
  point: lease-first markers complete before Close returns, while Closing-first attempts return
  `InvalidArgument`. The buffer receiver boundary is completion of StorageNode's serialized
  route/session/capability and durable or ephemeral application path. Applied and synchronized
  public receipts remain #107 scope, and synchronization composes with the #105 durability barrier.
* Batch visibility: StorageNode validates all entries before the first write and
  prevents subscriber messages from interleaving with batch entries. Queries do
  not take `subscriber_mutex`, so a concurrent Get/List may observe a partially
  applied batch
* Relationship with puts around `CreateSession`: the snapshot contains
  “the contents already reflected in the engine at the moment StorageNode processes CreateSession”.
  The caller must confirm completion of all required base writes before starting a session
  (because put passes through StorageNode receive processing, `ParamStore::Put` only reports
  Transport submission; acknowledgement and retry policy belong to Issues #14 and #17. §6.2)
* Buffer persistence and zenoh fanout are not atomic. StorageNode is one subscriber and cannot
  retract a sample already observed by another subscriber. A conflicting raw PUT can be observed
  live while durable Get retains the first bytes; it is protocol-invalid and must not be treated
  as a valid update. Exactly-once delivery is not promised.
* Durable late join is: declare a buffering subscriber, synchronously Get and materialize, drain
  buffered samples under one ordering boundary, then switch to live. Same-byte duplicates may be
  deduplicated during the transition. Ephemeral routes have no initial Get or replay.
* Reserve the SID in a non-queryable `Creating` record before external factory creation so failed
  creation leaves no active or queryable Session. A `Closing` record remains reserved until
  callbacks quiesce and all Session resources are released; only then can same-SID creation begin.
  Physical deletion is host-owned after CloseSession returns; #108 owns restart and
  retained-session catalog semantics. Orderly engine close/reopen checks validate resource release
  only and do not establish #108 restart or retention semantics.

### 4.4 Transport Integration Pseudocode

`PutOptions::ack` and `TransportSample::ack_token` already exist in the Transport API. The
`MetaAck` route and their end-to-end attachment, token, batch-outcome, UUID, and timeout semantics
remain planned Issue #14 work, as recorded in `docs/03_wire_protocol.md` §6. Implementers must not
treat the pseudocode as finalized acknowledgement behavior. #14 ACK routing, attachment, and
cache behavior are outside this pseudocode and must not be implemented until #14 finalizes them.

Pseudocode for implementers. StorageNode does not use the raw zenoh-c API directly; it goes through
the `Transport` abstraction ([09_dependency_policy.md](09_dependency_policy.md) §3).
`BuildBufferKey(prefix, sid, buffer_class, user_key)` emits the durable or ephemeral route
segment. `BufferClass` selects `durable` or `ephemeral`; `KeyKind::Buffer` and
`ParsedKey::buffer_class` identify parsed buffer routes, while non-buffer parsed keys leave
`buffer_class` disengaged. `AppendDiagnostic`
records a diagnostic without calling external code. `EmitDiagnostics` invokes the configured
`LogSink` only after the serialized application scope has released `subscriber_mutex`,
`session_mutex`, and every Session admission lease. `EmitLog` and `EmitDiagnosticsNoThrow` are
non-throwing helpers for the exception path. The callback-gate lease stays held through emission,
so `Stop()` still quiesces diagnostics. Neither callback ever invokes the external `LogSink` while
`subscriber_mutex`, `session_mutex`, or a Session admission lease is held.

`ParseQuerySelector` is a concise internal implementation helper, not a new public API. It composes
the exact-key grammar with the existing selector rules: it first uses `ParseKey(prefix, keyexpr)`
for an exact key, then parses a terminal `/**` selector with the existing selector parser. Issue
#141 implements the exact branch and selector branch for durable buffer exact Get/List. It returns
`kind`, `sid`, `buffer_class`, and a relative selector, or empty. Empty means no reply; ephemeral
selectors remain no-reply.

`AcquireActiveAdmission` below operates on the callback-captured `std::shared_ptr<State>`, not on
`StorageNode`. Start validates the configured prefix before the callback-shared State owns it;
the validated `prefix` is retained by that State, while `log_sink` is captured by value. Both
Transport callbacks acquire the node callback-gate lease before parsing or inspecting input. All
parse, engine, reply, and diagnostic accumulation is inside a try scope. Local locks and admission
leases therefore unwind before the catch and any diagnostic emission.

The synchronous-reentrancy boundary is explicit: `DurableBufferEngineFactory` and `LogSink` must
not synchronously call `Stop`, destruction, or another waiting lifecycle operation on the same
StorageNode. They must also not wait for a task or thread that does so. Non-blocking stop-request
posting and ordinary calls from an independent thread remain supported. This boundary does not
redesign the callback gate.

```cpp
QuerySelector ParseQuerySelector(prefix, keyexpr):  // internal; not public API
  if exact = ParseKey(prefix, keyexpr):
    if exact.is_batch: return empty
    return {exact.kind, exact.sid, exact.buffer_class, exact.relative_key}
  selector = ParseSelectorExpression(prefix, keyexpr)  // existing selector rules, including /**
  if selector.empty or selector.buffer_class == Ephemeral: return empty
  return {selector.kind, selector.sid, selector.buffer_class, selector.relative_selector}

StorageNode::Start(engine, transport, config):
  state = std::make_shared<State>()
  state->engine = engine
  state->prefix = config.prefix  // validated before State ownership
  state->durable_buffer_engine_factory = move(config.durable_buffer_engine_factory)
  log_sink = config.log_sink
  queryable_result = transport->DeclareQueryable(state->prefix + "/**",
    [state, log_sink](TransportQuery& q) {
      gate_lease = state->callback_gate.acquire()
      if not gate_lease: return
      try:
        selector = ParseQuerySelector(state->prefix, q.keyexpr)
        if not selector: return  // invalid or ephemeral query: normal no-reply
        switch (selector.kind):
          case Base:
            ReplyFromReader(q, *state->engine, selector.relative_selector)
          case Snapshot:
            if lease = AcquireActiveAdmission(*state, selector.sid):
              ReplyFromReader(q, *lease.session.snapshot, selector.relative_selector)
          case Session:
            if lease = AcquireActiveAdmission(*state, selector.sid):
              ReplyFromReader(q, *lease.session.overlay, selector.relative_selector)
          case Buffer:
            if selector.buffer_class == Durable:
              owned = []
              {
                if lease = AcquireActiveAdmission(*state, selector.sid):
                  if lease.session.options.durable_buffers:
                    // Collect while admission protects the engine and copy every view.
                    owned = CollectOwnedEntries(*lease.session.durable_buffers,
                                                selector.relative_selector)
              }  // release Session admission before any reply callback
              for entry in owned:
                q.Reply(entry.full_key, entry.bytes, Encoding{"zenoh/bytes"})
            else:
              // No replies: StorageNode retains no ephemeral state.
          case MetaSession:
            if lease = AcquireActiveAdmission(*state, selector.sid):
              q.Reply(q.keyexpr, SessionJson(selector.sid), kSitosV1)
      catch (...):
        EmitLog(log_sink, LogLevel::kError, kNodeComponent, kQueryCallbackFailed)
    })
  if queryable_result.is_error: return queryable_result.error
  queryable = move(queryable_result.value)

  subscriber_result = transport->DeclareSubscriber(state->prefix + "/**",
    [state, log_sink](const TransportSample& s) {
      gate_lease = state->callback_gate.acquire()
      if not gate_lease: return
      diagnostics = SubscriberDiagnostics{}
      try:
        {
          lock subscriber_lock(state->subscriber_mutex)
          if parsed = ParseKey(state->prefix, s.key):
            if s.kind == TransportSample::Kind::Delete:
              if parsed.kind == Base:
                state->engine->Delete(parsed.relative_key)
              else if parsed.kind == Session:
                if lease = AcquireActiveAdmission(*state, parsed.sid):
                  lease.session.overlay->Delete(parsed.relative_key)
              else if parsed.kind == Buffer:
                AppendDiagnostic(diagnostics, "buffer delete is unsupported")
              else:
                AppendDiagnostic(diagnostics, "read-only or unsupported delete")
            else if parsed.kind == Buffer:
              if parsed.is_batch or IsControlNamespace(parsed):
                AppendDiagnostic(diagnostics, "buffer control route is unsupported")
              else if parsed.buffer_class == Durable:
                if lease = AcquireActiveAdmission(*state, parsed.sid):
                  if not lease.session.options.durable_buffers:
                    AppendDiagnostic(diagnostics, "durable buffer capability is disabled")
                  else:
                    first_bytes = ReadExact(*lease.session.durable_buffers,
                                            parsed.relative_key)
                    if first_bytes is absent:
                      lease.session.durable_buffers->Put(parsed.relative_key, s.payload)
                    else if first_bytes == s.payload:
                      pass  // idempotent retry of the first stored bytes
                    else:
                      AppendDiagnostic(diagnostics,
                                       "conflicting durable buffer PUT is rejected")
              else if lease = AcquireActiveAdmission(*state, parsed.sid):
                if not lease.session.options.ephemeral_buffers:
                  AppendDiagnostic(diagnostics, "ephemeral buffer capability is disabled")
                else:
                  // Process no state: Zenoh fanout is independent of this subscriber.
                  pass
            else if parsed.is_batch:
              entries = DecodeBatch(s.payload)
              if parsed.kind == Session:
                if lease = AcquireActiveAdmission(*state, parsed.sid):
                  ApplyBatch(*state, parsed.kind, entries)
              else:
                ApplyBatch(*state, parsed.kind, entries)
            else if parsed.kind == Base:
              state->engine->Put(parsed.relative_key, s.payload)
            else if parsed.kind == Session:
              if lease = AcquireActiveAdmission(*state, parsed.sid):
                lease.session.overlay->Put(parsed.relative_key, s.payload)
            else:
              AppendDiagnostic(diagnostics, "read-only or unsupported put")
          else:
            AppendDiagnostic(diagnostics, "unsupported subscriber key")
        }  // subscriber/session locks and leases unwind before emission
        EmitDiagnostics(log_sink, diagnostics)
      catch (...):
        // Do not append to diagnostics here: allocation may fail while unwinding.
        EmitLog(log_sink, LogLevel::kError, kNodeComponent, kSubscriberCallbackFailed)
        EmitDiagnosticsNoThrow(log_sink, diagnostics)
    })
  if subscriber_result.is_error: reset(queryable); return subscriber_result.error
  subscriber = move(subscriber_result.value)
  // Commit both handles and activate the State at one linearization point.
  node.queryable_ = move(queryable)
  node.subscriber_ = move(subscriber)
  node.state_ = state
  state->Activate()
```

`ReplyFromReader` uses exact `Get` for a single-key selector and `List` for a terminal `/**`
selector. Durable buffer replies use the plain `zenoh/bytes` representation with no schema,
written as `Encoding{"zenoh/bytes"}`; no new public Encoding enum or constant is added. Ephemeral
queries produce no replies. Inside a queryable callback, do not wait; allow only short reads from
engine/overlay. Invalid query parsing is normal no-reply, not an exception diagnostic.

## 5. ParamCache (Subscriber Side)

### 5.1 Construction Sequence (Joining a Session)

`ParamCache` is a move-only session cache. Issue #19 provides Result-bearing local reads and
session-overlay writes; tests also use an internal test-access seam.

```text
ParamCache::Attach(sid):
  1. create an accepting candidate and declare <prefix>/session/<sid>/** (buffering);
  2. get <prefix>/snap/<sid>/** into a private snapshot baseline;
  3. get <prefix>/session/<sid>/** into a private overlay;
  4. under the delta sequence lock, build baseline + overlay, drain buffered samples,
     and atomically switch to live mode;
  5. publish the candidate only after the transaction succeeds.
```

The subscriber is declared before either Get. Every callback is either buffered or applied under
one gap-free sequence lock, and the application order is snapshot, overlay, buffered samples,
then live samples. A failed declaration, Get, or reply decode rolls back the candidate and leaves
the cache detached and retryable. A valid session with zero replies (including an unknown session,
which this protocol cannot distinguish from an empty one) attaches as an empty cache.

ParamCache is session-only: applications normally create the session before attaching, but
Attach validates only the session-id syntax. The current protocol does not perform a session
existence preflight, so an unknown or empty session may attach successfully with an empty cache.
`Detach()` closes callback admission, undeclares the subscription, waits for admitted callbacks,
and only then clears state. No callback mutates the cache after Detach returns.

### 5.2 Data Structures and Zero-Copy Reads [N01]

```cpp
class ParamCache {
    Result<std::shared_ptr<const ParamValue>> GetShared(std::string_view key) const;
    template <SupportedParamType T> Result<T> Get(std::string_view key) const;
    template <SupportedParamType T> Result<T> GetOr(std::string_view key, T default_value) const;
    template <ParamSpanElement T> Result<SpanHandle<T>> GetSpan(std::string_view key) const;
    Result<bool> Contains(std::string_view key) const;
    Result<void> List(std::string_view prefix, const ListSink& sink) const;
    Result<void> Put(std::string_view key, const ParamValue& value);
    template <ParamInput T> Result<void> Put(std::string_view key, T&& value);
    Result<void> PutBatch(std::span<const BatchEntry> entries);
};
```

* `ParamValue` holds `std::variant<bool, std::int64_t, double, std::string,
  std::vector<std::byte>>`.
* `SpanHandle<T>` returns a zero-copy span into a BYTES value and owns the immutable value
  by directly retaining shared ownership of the immutable `ParamValue`, surviving overwrite, Detach,
  move assignment, and destruction.
* Reader active-State publication uses atomic shared_ptr snapshots; map lookup uses a shared lock
  and immutable shared values. Delta application replaces values rather than mutating them.
* `GetOr` substitutes only `NotFound`; `List` uses raw-prefix matching, lexical order, and
  caller-thread callbacks after releasing cache locks.
* ParamCache writes use per-cache last-serialized-wins ordering. There is no self-echo
  deduplication or global ordering across caches. `PutBatch` preserves caller order and duplicate
  keys in one canonical message, but readers may observe partially applied batch entries because
  batch application is not reader-visible transaction isolation.

### 5.3 Session Overlay Deletion

In session mode, a DELETE removes the overlay and restores the snapshot baseline when one exists.
Base reads and writes use ParamStore's explicit `"base"` scope; ParamCache does not subscribe to
or query `base/**`.

## 6. SessionView (Host-Process Composite Read)

`SessionView` is a move-only, read-only in-process facade opened with
`SessionView::Open(const StorageNode&, sid)`. It performs no Transport operations and resolves the
active session overlay before its immutable snapshot. A snapshot value is used only when the
overlay does not contain the key; malformed selected payloads fail with `Status::Error` rather than
falling back.

`Open` captures the node State under `lifecycle_mutex_`, releases that mutex, and enters the
captured State's existing callback gate before inspecting session tables. Each later operation locks
its weak State, enters that gate, and under `session_mutex` copies the reader pair and weak
generation/owner identity. It then releases `session_mutex` and retains the Session admission lease
while using the copied readers for engine `Get`/`List`; `session_mutex` is never held during those
calls. The view stores only weak State and weak overlay-owner identities, so Stop, CloseSession,
and same-id recreation cannot leave a dangling or resource-pinning view. C++ shared-owner
identity, not raw pointer addresses, distinguishes session generations. CloseSession waits for
these leases before releasing Session resources.

`Get` returns an owned `ParamValue`; typed `Get`, `GetOr`, `Contains`, and `List` follow the same
Result/Status conventions as the client APIs. `List` materializes and validates the complete merged
set, applies raw-prefix semantics and lexical ordering, then releases the gate and engine locks
before invoking the caller sink. Sink false is a successful early stop and sink exceptions propagate
on the caller thread. `GetShared`, `GetSpan`, and all writes are intentionally absent. Large images
and compute artifacts belong to the route-selected `buffers/<sid>/durable/**` or
`buffers/<sid>/ephemeral/**` scopes (ADR-0032), not the session overlay or ParamCache.

## 7. ParamStore (Writes and Ad Hoc Reads)

### 7.1 API Semantics

* `Put(key, value)` / `PutBatch(entries)` — a batch is one `:batch` wire put
  (multi-entry format in the payload, [03](03_wire_protocol.md) §5) [F09]
* `Get<T>(key)` — synchronous exact zenoh get. Zero replies are `NotFound`.
* `Contains(key)` — exact get with zero replies mapped to `Ok(false)`.
* `List(prefix, sink)` — synchronous zenoh get using the narrowest safe chunk-boundary
  selector, followed by client-side raw-prefix filtering and lexical sorting.

### 7.2 Put Completion Guarantee

ParamStore write success means only that Transport accepted/submitted the operation. It does
not confirm StorageNode application, durability, or cache visibility. Acknowledged writes and
retry policy belong to Issues #14 and #17; this API does not add a `put_ack` configuration field.

### 7.3 ParamStore Subscription

`ParamStore::Subscribe` is a delta-only observer over base or session Transport samples. It performs
no initial read. Declaration-time samples are copied and staged; a successful declaration drains
staged work before returning, while declaration failure invokes no user callback. A move-only
`ParamSubscription` owns the native handle, callback state, shared Transport, and client LogSink
independently of ParamStore.

Each subscription has a threadless single-flight drainer. User callbacks are serialized per
subscription, canonical batches are prevalidated and expanded in encoded order without
interleaving, and duplicate/self-echo samples are not suppressed. `Close()` closes admission,
undeclares the native handle, waits for native callbacks, queued work, user callbacks, and diagnostics,
and guarantees no callback or LogSink invocation after return. Callback exceptions are contained and
logged. Callbacks may submit nonblocking writes but must not perform blocking reads or subscription
lifecycle operations from within the callback. This boundary is specified by ADR-0030; Python
callback dispatch remains Issue #26.

## 8. Session Lifecycle (Overall Sequence)

```
External client        Controller(StorageNode)          Calc(ParamCache)
     │ put base/** ────────►│ engine.Put                     │
     │ POST /job ──────────►│                                │
     │                      │ CreateSession(sid):            │
     │                      │   reserve SID; snapshot/overlay │
     │                      │   optional durable buffer       │
     │                      │   activate SessionRecord       │
     │                      │ spawn Calc(sid) ──────────────►│
     │                      │                                │ Attach(sid):
     │                      │◄─ get snap/<sid>/** ───────────│  initial fetch
     │                      │── replies ────────────────────►│  build cache
     │                      │                                │ (compute start)
     │ put base/** ────────►│ engine.Put                     │ ← no effect [F05]
     │                      │                                │
     │ (or Calc) ──────────►│ put session/<sid>/k ──(zenoh distributes to all subscribers)──►│ cache update [F06]
     │                      │   overlays[sid][k] = v         │
     │                      │                                │ (compute end)
     │ DELETE /job ────────►│ CloseSession(sid)              │ Detach()
     │                      │   close gate; destroy buffer    │
     │                      │   release snapshot/overlay [F10]│
```

## 9. Thread Model

| Component | Threads |
|---|---|
| zenoh callbacks (queryable/subscriber) | zenoh internal thread pool. sitos callbacks return quickly (engine I/O is allowed; blocking waits are prohibited) |
| Transport Get sink | zenoh reply callback. Sinks are serialized per request, must return quickly, and must not recursively call blocking Get on the same Transport (ADR-0020) |
| ParamCache delta application | zenoh subscriber thread. The writer lock is held only briefly for replacement |
| ParamCache local reads | Any application thread (atomic State snapshot + shared map lock) [N07] |
| ParamCache writes | Caller thread; submission occurs without lifecycle or map locks, then local sequencing |
| ParamSubscription callbacks | Whichever Transport callback/caller thread owns the per-subscription drainer; serialized per subscription, no thread affinity |
| Python callbacks | Dedicated dispatch thread + queue (the GIL is not acquired on zenoh threads) [P04] |

## 10. Error Handling Policy

* APIs return `bool` / `std::optional` / `sitos::Result<T>` (error code +
  message). Exceptions are used only for unrecoverable cases such as constructor failure
* Stale-state detection and reconnect recovery for ParamCache are future Issue #20 behavior;
  they are not provided by the current API.
* Type-mismatched Get: arithmetic casts are allowed among numeric types (BOOL/S64/DP) [C05];
  all other cases return failure

## 11. Configuration (Config)

```cpp
struct ClientConfig {
    std::string prefix = "sitos";
    std::optional<std::string> zenoh_config_json;  // complete zenoh Config (JSON5)
    std::chrono::milliseconds query_timeout{5000};
};
```

The default zenoh connection uses multicast scouting (same-host peers connect with zero configuration).
In environments where multicast cannot be used, specify explicit endpoints
(`connect.endpoints` / `listen.endpoints`) with `zenoh_config_json` [D13].

(END OF DOCUMENT)
