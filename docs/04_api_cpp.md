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
};
Result<void> ValidateClientConfig(const ClientConfig& config);

/// Payload format identifier ([03] §2.2). Corresponds to transport Encoding.
enum class Encoding { SitosV1, SitosV1Batch, Raw };

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
  ParamStore(ParamStore&&) noexcept = default;
  ParamStore& operator=(ParamStore&&) noexcept = default;

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

`scope` is `"base"`, `"session/<sid>"`, or `"snap/<sid>"`. Snapshot writes return
`Status::ReadOnly`; session Delete returns `Status::InvalidKey`. `Put`, `PutBatch`, and
`Delete` report Transport submission only and do not wait for node application. `PutBatch`
uses the canonical `:batch` key and sends one `sitos.v1.batch` message; an empty valid batch
sends no message.

`Get` waits for synchronous Transport completion. Zero replies map to `NotFound`, while
`Contains` maps them to `Ok(false)`. `List` collects and validates all matching replies,
sorts relative keys lexicographically, then invokes the sink on the caller thread. A false
sink result is normal early termination; sink exceptions propagate unchanged. Raw prefixes
are used: `foo` matches `foo`, `foo/bar`, and `foobar`, while `foo/` matches descendants only.

## 3. StorageEngine / StorageNode — Storage Node Side

`StorageEngine`/`StorageReader` are as described in [02_architecture.md](02_architecture.md) §3.
Bundled engines:

```cpp
/// Zero dependencies. TakeSnapshot performs a full copy [X02]
class InMemoryEngine final : public StorageEngine { ... };

/// Provided only when built with SITOS_WITH_ROCKSDB.
/// TakeSnapshot is O(1) via rocksdb::DB::GetSnapshot [N02]
class RocksDBEngine final : public StorageEngine {
public:
    static Result<std::unique_ptr<RocksDBEngine>> Open(const std::string& path);
};
```

```cpp
class StorageNode {
public:
    StorageNode() = default;
    explicit StorageNode(Transport& transport);
    Result<void> Start(std::shared_ptr<StorageEngine> engine, Config config);
    Result<void> Start(std::shared_ptr<StorageEngine> engine, Transport& transport,
                       Config config);

    /// In-process direct access to engine (no zenoh round trip).
    /// For fast reads in the host process (controller/orchestrator).
    const StorageEngine& engine() const;

    // ---- session management (equivalent to SessionController) ----
    Result<void> CreateSession(std::string_view sid);   // [F05]
    Result<void> CloseSession(std::string_view sid);    // [F10]
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

The internal cache uses immutable `shared_ptr<const ParamValue>` values and a shared mutex, but
public `GetShared`, scalar Get/GetOr, GetSpan/SpanHandle, Contains, List, Put, PutBatch, and
`stale()` belong to Issues #19/#20 and are not present in Issue #18.
## 5. SessionView — Composite View (Optional Use)

A facade for the host process side that handles overlay → snapshot resolution through one read
interface.

```cpp
class SessionView {
public:
    SessionView(const StorageNode& node, std::string_view sid);
    // Get/GetOr/GetSpan/Contains/List with the same shape as ParamCache (in-process resolution)
    // Put writes to the overlay + distributes via zenoh
};
```

## 6. Thread-Safety Contract

| Class | Contract |
|---|---|
| `ParamValue` | Immutable. Can be freely shared |
| `ParamStore` | All methods may be called concurrently |
| `ParamCache` | Attach/Detach lifecycle methods are serialized internally. Public read/reconnect semantics are deferred to Issues #19/#20 |
| `StorageNode` | All methods may be called concurrently |
| callback | Called from zenoh threads. Blocking is prohibited. From inside a callback, only Get-style APIs on the same object may be called |

(END OF DOCUMENT)
