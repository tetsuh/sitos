# sitos — C++ API Specification

namespace: `sitos::`. C++20. The header is `#include <sitos/sitos.hpp>` (umbrella).
Exceptions are used only for unrecoverable errors in constructors/factories. All other errors are represented by return values.

## 1. Basic Types

```cpp
namespace sitos {

/// Value type. Corresponds to the five payload v1 types ([03_wire_protocol.md] §2.1)
class ParamValue {
public:
    using Variant = std::variant<bool, std::int64_t, double,
                                 std::string, std::vector<std::byte>>;

    // Construction (numeric types are normalized to S64/DP: int32→S64, float→DP, etc.)
    template<typename T> explicit ParamValue(T&& v);

    ValueType type() const;   // enum class ValueType { Bool, S64, Dp, Str, Bytes }

    /// Typed extraction. Arithmetic casts are allowed among numeric types (Bool/S64/Dp).
    /// Impossible combinations return std::nullopt.
    template<typename T> std::optional<T> As() const;

    /// View a Bytes value as an array of T (zero-copy). T is trivially copyable.
    /// Returns std::nullopt if the value is not Bytes or its size is not a multiple of sizeof(T).
    template<typename T> std::optional<std::span<const T>> AsSpan() const;

    /// Encode/decode to/from payload v1
    std::vector<std::byte> Encode() const;
    static std::optional<ParamValue> Decode(std::span<const std::byte> payload);
};

/// Result with an exclusive value or ErrorInfo state. See include/sitos/result.hpp.
template<typename T>
class Result {
public:
    static Result Ok(T value);
    static Result Err(std::error_code cause);
    static Result Err(Status status, std::string message = {},
                      std::error_code cause = {});
    template<typename U> static Result ErrFrom(const Result<U>& source);
    bool IsOk() const noexcept;
    Status StatusCode() const noexcept;
    std::string_view Message() const noexcept;
    const T& Value() const &;  // Requires IsOk().
    T& Value() &;              // Requires IsOk().
    T&& Value() &&;            // Requires IsOk().
    const std::error_code& Error() const;  // Requires !IsOk().
};

/// Result<void> has the same status/error observers without a value.
template<> class Result<void>;

// Message() borrows the diagnostic string. Its view remains valid only while
// the Result and its error state live, and assignment or move invalidates it.

/// Public error state containing stable classification, diagnostics, and native cause.
struct ErrorInfo {
    Status status;
    std::string message;
    std::error_code cause;
};

// Numeric values are stable because MakeErrorCode publishes them.
enum class Status {
    Ok = 0,
    NotFound = 1,
    TypeMismatch = 2,
    Timeout = 3,
    Disconnected = 4,
    ReadOnly = 5,
    InvalidKey = 6,
    InvalidArgument = 7,
    Error = 8
};
const std::error_category& StatusErrorCategory() noexcept;
std::error_code MakeErrorCode(Status status);

/// Shared client configuration. Empty JSON is invalid; nullopt selects Zenoh defaults.
struct ClientConfig {
    std::string prefix = "sitos";
    std::optional<std::string> zenoh_config_json;
    std::chrono::milliseconds query_timeout{5000};
    std::shared_ptr<LogSink> log_sink = DefaultLogSink();
};
Result<void> ValidateClientConfig(const ClientConfig& config);

/// Payload format identifier ([03] §2.2). Corresponds to transport Encoding.
struct Encoding {
    static constexpr std::string_view kSitosV1 = "sitos.v1";
    static constexpr std::string_view kSitosV1Batch = "sitos.v1.batch";
    std::string id;
};

/// Common sink for List APIs. Returning false aborts enumeration.
using ListSink = std::function<bool(std::string_view key, const ParamValue&)>;

/// RAII handle for subscriptions/queryables. Destruction undeclares it.
class Subscription;
class Queryable;

} // namespace sitos
```

Key arguments are `std::string_view` in all APIs. Invalid keys ([03] §1.2) produce
`Status::InvalidKey`.

The `Transport` abstraction (a zenoh adapter that provides put/get/queryable/subscriber) is
defined in [09_dependency_policy.md](09_dependency_policy.md) §3. Its Get timeout must be
strictly positive; successful Get returns after terminal reply completion with no subsequent
sink callback (ADR-0020). `ParamStore`/`ParamCache`/`StorageNode` do not expose raw zenoh-c
types in the public API. An injected `std::shared_ptr<Transport>` can be passed directly to
`ParamStore::Open`; configuration-aware Zenoh session creation remains an internal factory detail.

### 1.1 Status / Python Exception Mapping

| Status | C++ condition | Python exception |
|---|---|---|
| `Ok` | Success | None |
| `NotFound` | get target absent, nonexistent session | `sitos.NotFoundError` |
| `TypeMismatch` | Type conversion impossible, Bytes dtype/size mismatch | `sitos.TypeMismatchError` |
| `Timeout` | query does not complete within `ClientConfig::query_timeout` | `sitos.TimeoutError` |
| `Disconnected` | zenoh session disconnected, StorageNode stopped | `sitos.DisconnectedError` |
| `ReadOnly` | put/delete through the library API to `snap/<sid>/**` | `sitos.ReadOnlyError` |
| `InvalidKey` | Key/scope/session id violates the grammar | `ValueError` |
| `InvalidArgument` | Invalid configuration or operation argument | `ValueError` |
| `Error` | Other implementation-dependent error (RocksDB status, etc.) | `sitos.SitosError` |

Python `get(..., default=...)` does not raise for `NotFound` only; it returns default.
All other Status values are converted to exceptions.

## 2. ParamStore — Writes and Ad Hoc Reads

```cpp
class ParamStore {
 public:
  static Result<ParamStore> Open(ClientConfig config = {});
  static Result<ParamStore> Open(std::shared_ptr<Transport> transport,
                                 ClientConfig config = {});

  ParamStore(const ParamStore&) = delete;
  ParamStore& operator=(const ParamStore&) = delete;
  ParamStore(ParamStore&&) noexcept;
  ParamStore& operator=(ParamStore&&) noexcept;

  Result<ParamSubscription> Subscribe(std::string_view scope,
                                      std::string_view prefix,
                                      ParamCallback callback);

  Result<void> Put(std::string_view scope, std::string_view key,
                   const ParamValue& value);
  template <ParamInput T>
  Result<void> Put(std::string_view scope, std::string_view key, T&& value);
  Result<void> PutBatch(std::string_view scope,
                        std::span<const BatchEntry> entries);
  Result<void> Delete(std::string_view scope, std::string_view key);

  Result<ParamValue> Get(std::string_view scope, std::string_view key);
  template <SupportedParamType T>
  Result<T> Get(std::string_view scope, std::string_view key);
  Result<bool> Contains(std::string_view scope, std::string_view key);
  Result<void> List(std::string_view scope, std::string_view prefix,
                    const ListSink& sink);
};
```

`scope` is `"base"`, `"session/<sid>"`, or `"snap/<sid>"`. The public `Delete` API is
base-only; session Delete returns `Status::InvalidKey`, and snapshot writes return
`Status::ReadOnly`. Raw Transport DELETE remains supported for both base and session routes;
buffer DELETE is unsupported in v0.4. `Put`, `PutBatch`, and `Delete` report Transport submission
only and do not wait for node application. `PutBatch`
uses the canonical `:batch` key and sends one `sitos.v1.batch` message; an empty valid batch
sends no message.

`Get` waits for synchronous Transport completion. Zero replies map to `NotFound`, while
`Contains` maps them to `Ok(false)`. `List` collects and validates all matching replies,
sorts relative keys lexicographically, then invokes the sink on the caller thread. A false
sink result is normal early termination; sink exceptions propagate unchanged. Raw prefixes
are used: `foo` matches `foo`, `foo/bar`, and `foobar`, while `foo/` matches descendants only.

### 2.1 ParamStore subscriptions

`Subscribe` accepts only `base` and syntactically valid `session/<sid>` scopes. It is delta-only:
it performs no initial Get/List. Matching PUT and DELETE samples produce owned relative-key
`ParamChange` events. Canonical `:batch` PUTs are fully validated before delivery and expand into
ordered individual PUT events, preserving duplicates. Unknown ordinary encodings are delivered as
BYTES; malformed known payloads, invalid batches, and unsupported paths are dropped with diagnostics.

`ParamSubscription` is move-only. Declaration-time samples are staged and drained only after
successful declaration. `Close()` is synchronous, idempotent, and callback-quiescent. It waits for
native callbacks, queued work, user callbacks, and diagnostics; no callback or LogSink call starts
after it returns. Callbacks are serialized per subscription but have no thread affinity. They may
submit nonblocking Put/PutBatch/Delete, but must not call blocking reads or close/destroy the
subscription from inside its callback. Python callback dispatch is Issue #26.

## 3. StorageEngine / StorageNode — Storage Node Side

`StorageEngine`/`StorageReader` are as described in [02_architecture.md](02_architecture.md) §3.
Bundled engines:

```cpp
/// Zero dependencies. TakeSnapshot performs a full copy [X02]
class InMemoryEngine final : public StorageEngine { ... };

/// The PImpl-only public API is installed in both build modes. A functional implementation requires
/// SITOS_WITH_ROCKSDB=ON; the OFF build returns Status::Error with operation_not_supported.
/// TakeSnapshot consumes ADR-0004's O(1), copy-free native snapshot invariant [N02].
class RocksDBEngine : public StorageEngine {
public:
    static Result<std::unique_ptr<RocksDBEngine>> Open(const std::string& path);
};
```

The RocksDB-ON installed package reconstructs the exact RocksDB version used at build time. The
installed consumer validates configure and compile/link only; runtime deployment remains with the
application or package manager. Exact version equality is necessary but not sufficient for ABI
compatibility. See ADR-0033.

```cpp
struct SessionOptions {
    bool durable_buffers = false;
    bool ephemeral_buffers = false;
};

using DurableBufferEngineFactory =
    std::function<Result<std::unique_ptr<StorageEngine>>(std::string_view sid)>;

struct StorageNodeConfig {
    std::string prefix = "sitos";
    /// Diagnostic destination; nullptr explicitly disables logging.
    std::shared_ptr<LogSink> log_sink = DefaultLogSink();
    DurableBufferEngineFactory durable_buffer_engine_factory = {};
};

class StorageNode {
public:
    using Config = StorageNodeConfig;

    StorageNode() = default;
    explicit StorageNode(Transport& transport);
    Result<void> Start(std::shared_ptr<StorageEngine> engine, Config config);
    Result<void> Start(std::shared_ptr<StorageEngine> engine, Transport& transport,
                       Config config);

    // ---- session management (equivalent to SessionController) ----
    // The one-argument overload preserves the existing no-buffer behavior.
    Result<void> CreateSession(std::string_view sid);  // [F05]
    Result<void> CreateSession(std::string_view sid, SessionOptions options);  // [F05]
    Result<void> CloseSession(std::string_view sid);   // [F10]
    std::vector<std::string> ActiveSessions() const;

    void Stop() noexcept;   // quiesces callbacks, then undeclares queryable/subscriber

    StorageNode(const StorageNode&) = delete;
    StorageNode& operator=(const StorageNode&) = delete;
    StorageNode(StorageNode&&) = delete;
    StorageNode& operator=(StorageNode&&) = delete;
};

// Start stages both Transport declarations and activates the node only after
// both succeed. Stop is idempotent and waits for callbacks already in flight.
```

`SessionOptions` enables durable buffers, ephemeral buffers, both, or neither. The exact lifecycle
contract is `absent → Creating → Active` for `CreateSession` and
`Active → Closing → absent` for `CloseSession`. Creation against `Creating` or `Closing` returns
`std::errc::operation_in_progress`; a duplicate `Active` creation retains
`std::errc::file_exists`. Close against `Creating` or `Closing` returns
`std::errc::operation_in_progress`, while missing Close retains
`std::errc::no_such_file_or_directory`. The creator commits only after verifying under
`session_mutex` that the same reservation still exists and remains `Creating`.

A node-level host factory creates at most one durable `StorageEngine` for each Session that enables
the durable capability; all durable keys in that Session share it. The factory result is uniquely
owned by the Session, and RocksDB types and filesystem paths remain outside this public API.
`Start` moves the factory from `StorageNodeConfig` into the callback-shared State before either
Transport declaration. `CreateSession` uses that same State generation, not a later or different
node configuration. Creation reserves a non-queryable `Creating` record before invoking the
external factory, rolls back every resource already created on failure, and publishes it as
`Active` only after all resources are ready.

Only a valid, non-null engine may commit `Creating` to `Active`. For a Session requesting
durable buffers, a factory `Err` preserves its status, cause, and message. An empty factory,
`Ok(nullptr)`, or a factory exception fails with a non-OK Result; exceptions are contained. The
exact Status taxonomy for empty, null, and exception outcomes is defined by
DEC-56-FACTORY-FAILURE-001 and implemented in stage #141; it is not deferred to the final #56
integration. The non-commit path releases resources created by that attempt and removes only the
same `Creating` reservation, allowing same-SID retry.
The `session_mutex` is not held during external factory or engine operations, callback
quiescence, resource destruction, or logging. Close leaves a `Closing` record reserved until
callbacks quiesce and every Session-owned resource, including the durable engine, is released.
Same-SID lifecycle phase collisions do not wait and return
`std::errc::operation_in_progress`; admission quiescence is a separate required wait and may use
the gate's normal synchronization implementation. This contract does not prescribe
condition-variable internals. Physical directory removal is host-owned after return.

The synchronous-reentrancy boundary applies to `DurableBufferEngineFactory` and `LogSink`: neither
may synchronously call `Stop`, destruction, or another waiting lifecycle operation on the same
StorageNode, or wait for a task or thread that does so. Non-blocking stop-request posting and
ordinary calls from an independent thread remain supported. The callback gate is unchanged.

`ActiveSessions()` takes the node callback-gate lease, then takes `session_mutex` and copies only
SIDs whose records are `SessionPhase::Active`. It releases `session_mutex` and the gate lease
before returning, retains no Session resources after enumeration, and never externally lists
`Creating` or `Closing` records.

Existing `KeyKind` enumerator values remain unchanged, and existing five-field positional
`ParsedKey` aggregate initialization remains valid because `buffer_class` is appended with a
`= std::nullopt` default. Five-element structured bindings and exhaustive `KeyKind` switches may
require source changes when they need to handle the new Buffer kind. The existing
`CreateSession(sid)` source call remains a distinct overload with member-pointer shape
`Result<void> (StorageNode::*)(std::string_view)`. Adding `durable_buffer_engine_factory` extends
the public `StorageNodeConfig` layout; no binary ABI compatibility is promised, and consumers must
rebuild against the updated library or package. Issue #56 adds no C++ or Python BufferPublisher or
BufferSubscriber API. The public C++ engine-factory API and SessionOptions boundary are implemented
in stage #141; no Python engine-factory API is added. Buffer values use the route-selected wire
contract in ADR-0032, not ParamStore or ParamCache APIs. The key API uses
`BufferClass { Durable, Ephemeral }`, appends `KeyKind::Buffer`, and exposes
`std::optional<BufferClass> ParsedKey::buffer_class`, engaged only for buffer routes.
`BuildBufferKey(prefix, sid, buffer_class, user_key)` returns
`<prefix>/buffers/<sid>/{durable,ephemeral}/<key>` and returns `std::nullopt` for invalid
components or an undefined enum value. Existing non-buffer parsed keys leave `buffer_class`
disengaged.

## 4. ParamCache — Subscriber-Side Hot Path

```cpp
class ParamCache {
 public:
  static Result<ParamCache> Open(ClientConfig config = {});
  static Result<ParamCache> Open(std::shared_ptr<Transport> transport,
                                 ClientConfig config = {});

  ~ParamCache();
  ParamCache(const ParamCache&) = delete;
  ParamCache& operator=(const ParamCache&) = delete;
  ParamCache(ParamCache&&) noexcept;
  ParamCache& operator=(ParamCache&&) noexcept;

  Result<void> Attach(std::string_view sid);
  void Detach() noexcept;
};
```

Issue #18 provides only construction and attachment lifecycle. ParamCache attaches only to an
explicit syntactically valid session id. It does not perform a session-existence preflight: a
valid unknown or empty session may attach as an empty cache because the current protocol cannot
distinguish those cases. Attach declares the subscriber before synchronously fetching snapshot and
overlay data, buffers subscriber samples during the transaction, then drains that buffer and
switches to live mode atomically. Failed declarations, transport errors, malformed replies, and
invalid keys roll back all candidate state; a retry starts from detached state. Detach closes
callback admission, undeclares the subscription, waits for in-flight callbacks, and then clears
state. Base reads and writes use ParamStore's explicit `"base"` scope; ParamCache does not expose
or subscribe to a base attachment mode.

The internal cache uses immutable `shared_ptr<const ParamValue>` values and a shared mutex.
Issue #19 adds the Result-bearing local API and session-overlay writes:

```cpp
Result<std::shared_ptr<const ParamValue>> GetShared(std::string_view key) const;
template <SupportedParamType T> Result<T> Get(std::string_view key) const;
template <SupportedParamType T> Result<T> GetOr(std::string_view key, T fallback) const;
template <ParamSpanElement T> Result<SpanHandle<T>> GetSpan(std::string_view key) const;
Result<bool> Contains(std::string_view key) const;
Result<void> List(std::string_view prefix, const ListSink& sink) const;
Result<void> Put(std::string_view key, const ParamValue& value);
template <ParamInput T> Result<void> Put(std::string_view key, T&& value);
Result<void> PutBatch(std::span<const BatchEntry> entries);
```

Reads are cache-local and perform no Transport operation or payload deep copy. `List` uses raw
prefix semantics (`foo` includes `foo`, `foo/bar`, and `foobar`; `foo/` includes descendants),
sorts lexically, and invokes the caller's sink after releasing internal locks. `GetSpan` returns
a `SpanHandle` owning its immutable value through overwrite, Detach, move assignment, and cache
destruction. Writes submit to the attached session first, then apply locally on success; peers
receive updates asynchronously through the subscriber. Failed submission performs no local mutation.
Writes use per-cache last-serialized-wins ordering; there is no self-echo deduplication or global
ordering across caches. `PutBatch` preserves caller order and duplicate keys in one canonical
message, and is not reader-visible transaction isolation, so concurrent readers may observe
partial application. All APIs use the Result/Status model; `GetOr` substitutes only `NotFound`.
`WaitForLocalDelivery` is deferred to #99, and stale/reconnect behavior is future #20 behavior.
## 5. SessionView — Read-Only Composite View

`SessionView` is the host-process facade for an active session. It is opened through the Result-based
factory and does not perform Transport operations or writes.

```cpp
class SessionView {
 public:
  static Result<SessionView> Open(const StorageNode& node, std::string_view sid);

  Result<ParamValue> Get(std::string_view key) const;
  template <SupportedParamType T> Result<T> Get(std::string_view key) const;
  template <SupportedParamType T> Result<T> GetOr(std::string_view key, T default_value) const;
  Result<bool> Contains(std::string_view key) const;
  Result<void> List(std::string_view prefix, const ListSink& sink) const;
};
```

Reads resolve overlay before snapshot and fall back only when the overlay key is absent. A malformed
selected payload returns `Status::Error`. `List` materializes and validates the merged set, sorts
keys lexically using raw-prefix matching, releases internal synchronization, and then invokes the
caller sink. `GetShared`, `GetSpan`, `Put`, `PutBatch`, and `Delete` are intentionally absent.
Large binary values belong to the route-selected `buffers/<sid>/durable/**` or
`buffers/<sid>/ephemeral/**` scopes described by ADR-0032. These routes are not SessionView data.

## 6. Thread-Safety Contract

| Class | Contract |
|---|---|
| `ParamValue` | Immutable. Can be freely shared |
| `ParamStore` | All methods may be called concurrently |
| `ParamCache` | Attach/Detach and local write sequencing are synchronized internally. Local reads are cache-only; stale/reconnect behavior is future #20 behavior |
| `StorageNode` | Ordinary independent-thread calls may run concurrently; `DurableBufferEngineFactory` and `LogSink` must not synchronously call `Stop`, destruction, or another waiting lifecycle operation on the same node, or wait for one |
| `SessionView` | All methods may be called concurrently. List callbacks run on the caller thread outside internal locks; re-entry and Stop from inside a sink are safe |
| ParamSubscription callback | Serialized per subscription with no thread affinity. It may submit nonblocking Put/PutBatch/Delete, but blocking reads, Subscribe, subscription lifecycle, and Transport/session lifecycle operations are forbidden |

(END OF DOCUMENT)
