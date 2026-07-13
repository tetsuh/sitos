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
 │  SessionController (session creation/destruction)          │   │
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
| `StorageNode` | Connects zenoh queryable/subscriber ↔ engine. Manages overlay/snapshot lifecycles | zenoh, StorageEngine |
| `ParamStore` | Client API: typed Put/Get/List/Delete/Subscribe. Wraps a zenoh session | zenoh |
| `ParamCache` | Subscriber-side read cache. Initial fetch + delta subscription + zero-copy Get | zenoh |
| `SessionController` | Session creation, snapshot registration, and destruction (inside the process that owns StorageNode) | StorageNode |
| `SessionView` | Logical view that resolves overlay → snapshot (thin facade over ParamStore/ParamCache) | ParamStore or ParamCache |

**Design principle**: `engine/` does not know about zenoh. `ParamStore`/`ParamCache` do not
know about engine. Direct dependencies on the zenoh-cpp API are confined to the
`transport/zenoh` layer, and higher-level components see only the `Transport` abstraction.
This limits the impact scope when zenoh is upgraded ([09_dependency_policy.md](09_dependency_policy.md)).

## 2. Key Space

Default prefix: `sitos` (configurable [X03]). For the detailed grammar and normalization rules,
see [03_wire_protocol.md](03_wire_protocol.md).

```
<prefix>/base/<key...>                 # master data
<prefix>/session/<sid>/<key...>        # session overlay (Put during execution)
<prefix>/snap/<sid>/<key...>           # read view of the session snapshot (read-only)
<prefix>/buffers/<sid>/<key...>        # session-scoped, disk-backed large values (pull+push) [ADR-0014]
<prefix>/meta/session/<sid>            # session metadata (state, creation time)
```

* put to `base/**` → the StorageNode subscriber writes to the engine [F04]
* get to `base/**` → the StorageNode queryable responds from the engine [F03, F08]
* put to `session/<sid>/**` → StorageNode records it in the overlay (an in-memory map
  separate from the engine). zenoh directly distributes it to subscribers (ParamCache) [F06]
* get to `snap/<sid>/**` → StorageNode responds from the snapshot view.
  put/delete through the library API returns a `ReadOnly` error; raw zenoh put/delete is
  fire-and-forget, so it is ignored + a warning is logged [F05, F08]
* put to `buffers/<sid>/**` → for a `kDurable` session the StorageNode subscriber writes bytes
  to the per-session buffer engine, and zenoh distributes the same bytes to live subscribers
  (full-payload push); for `kEphemeral` nothing is stored [ADR-0014]
* get to `buffers/<sid>/**` → for `kDurable`, the StorageNode queryable responds from the
  per-session buffer engine; for `kEphemeral`, not-found. Buffers use no snapshot view [ADR-0014]
* buffers live for the session lifetime: get-able from `CreateSession` until `CloseSession`,
  which purges the per-session buffer store [ADR-0014]

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
   - `buffers/<sid>/**` → per-session buffer engine (`kDurable`); not-found (`kEphemeral`) [ADR-0014]
2. Declare zenoh `subscriber`: receive put/delete for `<prefix>/**`
   - `base/**` → apply to engine
   - `session/<sid>/**` → apply to overlay
   - put to `snap/**` → ignore + warning log (read-only)
   - `buffers/<sid>/**` → per-session buffer engine (`kDurable`); no store (`kEphemeral`) [ADR-0014]
3. Session lifecycle (via SessionController):
   - `CreateSession(sid)`: obtain `engine->TakeSnapshot()` and register it in
     `snapshots_[sid]`. Create an empty `overlays_[sid]`. For buffers, create a
     per-session disk buffer engine when the session is `kDurable` [ADR-0014]
   - `CloseSession(sid)`: delete both tables (release shared_ptr) [F10], and purge
     the per-session buffer engine [ADR-0014]

### 4.2 Data Structures

```cpp
class StorageNode {
    std::shared_ptr<StorageEngine> engine_;
    // sid → snapshot view
    std::unordered_map<std::string, std::shared_ptr<const StorageReader>> snapshots_;
    // sid → overlay (key → payload byte sequence)
    std::unordered_map<std::string, OverlayMap> overlays_;
    std::shared_mutex mutex_;  // protects table structure (each overlay locks itself internally)
};
```

### 4.3 Consistency Model

* Write ordering: zenoh preserves the order of puts from the same publisher.
  It does not guarantee ordering across different publishers (last-write-wins)
* Relationship with puts around `CreateSession`: the snapshot contains
  “the contents already reflected in the engine at the moment StorageNode processes CreateSession”.
  The caller must confirm completion of all required base writes before starting a session
  (because put passes through StorageNode receive processing,
  `ParamStore::Put` by default performs a round-trip confirmation (ack) to the queryable. §6.2)

### 4.4 Transport Integration Pseudocode

Pseudocode for implementers. StorageNode does not use the raw zenoh-cpp API directly;
it goes through the `Transport` abstraction ([09_dependency_policy.md](09_dependency_policy.md) §3).

```cpp
StorageNode::Start(engine, transport, config):
  node.engine_ = engine
  queryable_result = transport->DeclareQueryable(config.prefix + "/**",
    [node](TransportQuery& q) {
      ParsedKey k = ParseKey(q.keyexpr)
      switch (k.kind):
        case Base:
          ReplyFromReader(q, *node.engine_, k.relative_key)
        case Snapshot:
          reader = node.FindSnapshot(k.sid)
          if reader: ReplyFromReader(q, *reader, k.relative_key)
        case Session:
          overlay = node.FindOverlay(k.sid)
          if overlay: ReplyFromOverlay(q, *overlay, k.relative_key)
        case MetaAck:
          if node.ack_cache.contains(k.uuid): q.Reply(k.keyexpr, OkPayload(), kSitosV1)
        case MetaSession:
          if node.sessions.contains(k.sid): q.Reply(k.keyexpr, SessionJson(k.sid), kSitosV1)
    })
  if queryable_result.is_error: return queryable_result.error
  queryable = move(queryable_result.value)

  subscriber_result = transport->DeclareSubscriber(config.prefix + "/**",
    [node](const TransportSample& s) {
      ParsedKey k = ParseKey(s.key)
      if s.kind == TransportSample::Kind::Delete:
        if k.kind == Base: node.engine_->Delete(k.relative_key)
        else if k.kind == Session: node.overlay(k.sid).Delete(k.relative_key)
        else LogWarn("read-only or unsupported delete")
        return

      if k.is_batch:
        entries = DecodeBatch(s.payload)
        ApplyBatch(k.scope, entries)
      else:
        if k.kind == Base: node.engine_->Put(k.relative_key, s.payload)
        else if k.kind == Session: node.overlay(k.sid).Put(k.relative_key, s.payload)
        else LogWarn("read-only or unsupported put")

      if s.ack_token: node.ack_cache.insert(*s.ack_token)
    })
  if subscriber_result.is_error: reset(queryable); return subscriber_result.error
  // Commit both handles and activate the State at one linearization point.
  node.queryable_ = move(queryable)
  node.subscriber_ = move(subscriber_result.value)
  node.state_ = state
  state.Activate()
```

`ReplyFromReader` uses `Get` for single-key queries and `List` for `**` queries.
Inside a queryable callback, do not wait; allow only short reads from engine/overlay.

## 5. ParamCache (Subscriber Side)

### 5.1 Construction Sequence (Joining a Session)

```
ParamCache::Attach(sid):
  1. declare subscriber: <prefix>/session/<sid>/**     … (a)
  2. get: <prefix>/snap/<sid>/**    → store in cache   … (b)
  3. get: <prefix>/session/<sid>/** → overwrite cache  … (c)
  4. apply the buffer already received by (a), then apply sequentially thereafter
```

Because the order is (a)→(b)→(c), puts that occur between (b) and (c) are also picked up by (a),
so there are no missed updates (the same technique as zenoh Querying Subscriber).
Duplicate application is idempotent because it overwrites the same key.

### 5.2 Data Structures and Zero-Copy Reads [N01]

```cpp
class ParamCache {
    // Key → decoded value. Values are held by shared_ptr,
    // so readers can safely keep references outside the lock section.
    std::unordered_map<std::string, std::shared_ptr<const ParamValue>> map_;
    mutable std::shared_mutex mutex_;
};
```

* `ParamValue` holds `std::variant<bool, std::int64_t, double, std::string,
  std::vector<std::byte>>`
* `Get<std::span<const float>>(key)` returns a span pointing to the internal buffer of a BYTES value
  via shared_ptr aliasing — no copy, lifetime-safe
* Delta application (writer) only replaces the value with a new `shared_ptr<const ParamValue>`.
  Old values held by existing readers are protected by shared_ptr

### 5.3 Direct Base Reference Mode

For simple use cases without sessions, also provide a mode where `Attach("base")`
initially fetches + subscribes to `base/**` (no snapshot isolation).

## 6. ParamStore (Writes and Ad Hoc Reads)

### 6.1 API Semantics

* `Put(key, value)` / `Put(entries)` — a batch is one zenoh put
  (multi-entry format in the payload, [03](03_wire_protocol.md) §5) [F09]
* `Get<T>(key)` — zenoh get (round trip). Not for the hot path
* `List(prefix, sink)` — zenoh get `<prefix>**`
* `Subscribe(pattern, callback)` — application-facing subscription [F13]

### 6.2 Put Completion Guarantee

Because zenoh put is fire-and-forget, provide an option that guarantees
“Put complete = reflected in StorageNode”:

* `PutOptions::ack = true` (default): after put, get the StorageNode ack queryable
  (`<prefix>/meta/ack/...`) to confirm reflection
* `ack = false`: prioritize low latency (for example, in-flight puts to a session overlay,
  where ordering alone is sufficient)

## 7. Session Lifecycle (Overall Sequence)

```
External client        Controller(StorageNode)          Calc(ParamCache)
     │ put base/** ────────►│ engine.Put                     │
     │ POST /job ──────────►│                                │
     │                      │ CreateSession(sid):            │
     │                      │   snap = engine.TakeSnapshot() │
     │                      │   overlays[sid] = {}           │
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
     │                      │   delete snapshots/overlays [F10]│
```

## 8. Thread Model

| Component | Threads |
|---|---|
| zenoh callbacks (queryable/subscriber) | zenoh internal thread pool. sitos callbacks return quickly (engine I/O is allowed; blocking waits are prohibited) |
| ParamCache delta application | zenoh subscriber thread. The writer lock is held only briefly for replacement |
| ParamCache Get | Any application thread (shared lock) [N07] |
| Python callbacks | Dedicated dispatch thread + queue (the GIL is not acquired on zenoh threads) [P04] |

## 9. Error Handling Policy

* APIs return `bool` / `std::optional` / `sitos::Result<T>` (error code +
  message). Exceptions are used only for unrecoverable cases such as constructor failure
* On zenoh disconnection: ParamCache raises a stale flag, then recovers after reconnection
  by performing the equivalent of Attach again [N10]
* Type-mismatched Get: arithmetic casts are allowed among numeric types (BOOL/S64/DP) [C05];
  all other cases return failure

## 10. Configuration (Config)

```cpp
struct Config {
    std::string prefix = "sitos";
    std::optional<std::string> zenoh_config_json;  // zenoh Config (JSON5)
    bool put_ack = true;
    std::chrono::milliseconds query_timeout{5000};
};
```

The default zenoh connection uses multicast scouting (same-host peers connect with zero configuration).
In environments where multicast cannot be used, specify explicit endpoints
(`connect.endpoints` / `listen.endpoints`) with `zenoh_config_json` [D13].

(END OF DOCUMENT)
