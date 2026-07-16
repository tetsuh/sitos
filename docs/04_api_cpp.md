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

struct Config {
    std::string prefix = "sitos";
    std::optional<std::string> zenoh_config_json;  // zenoh Config (JSON5)
    bool put_ack = true;
    std::chrono::milliseconds query_timeout{5000};
};

/// Payload format identifier ([03] §2.2). Corresponds to transport Encoding.
enum class Encoding { SitosV1, SitosV1Batch, Raw };

/// Put completion guarantee option ([02] §6.2)
struct PutOptions {
    bool ack = true;
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
sink callback (ADR-0020). `ParamStore`/`ParamCache`/`StorageNode` do not expose raw zenoh-cpp
types in the public API.
zenoh session injection [X04] is performed by converting the session to
`std::shared_ptr<Transport>` with the `ZenohTransport::From(session)` factory before passing it in.

### 1.1 Status / Python Exception Mapping

| Status | C++ condition | Python exception |
|---|---|---|
| `Ok` | Success | None |
| `NotFound` | get target absent, nonexistent session | `sitos.NotFoundError` |
| `TypeMismatch` | Type conversion impossible, Bytes dtype/size mismatch | `sitos.TypeMismatchError` |
| `Timeout` | query/ack does not complete within `Config::query_timeout` | `sitos.TimeoutError` |
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
    /// Open a new zenoh session (creates ZenohTransport internally)
    static Result<ParamStore> Open(const Config& config);
    /// Share an existing Transport [X04]
    static Result<ParamStore> Open(std::shared_ptr<Transport> transport,
                                   const Config& config);

    // ---- Writes (scope: "base" or "session/<sid>") ----
    Result<void> Put(std::string_view scope, std::string_view key,
                     const ParamValue& value);
    template<typename T>
    Result<void> Put(std::string_view scope, std::string_view key, T&& value);
    /// Batch [F09]: atomically delivered in one zenoh put
    Result<void> PutBatch(std::string_view scope,
                          std::span<const std::pair<std::string, ParamValue>> entries);
    Result<void> Delete(std::string_view scope, std::string_view key); // base only [F12]

    // ---- Reads (round trip. Use ParamCache for the hot path) ----
    Result<ParamValue> Get(std::string_view scope, std::string_view key);
    template<typename T>
    Result<T> Get(std::string_view scope, std::string_view key);
    bool Contains(std::string_view scope, std::string_view key);      // [F11]

    /// Enumerate under prefix (chunk boundary). Abort if sink returns false. [F03]
    Result<void> List(std::string_view scope, std::string_view prefix,
                      const ListSink& sink);

    // ---- Subscription [F13] ----
    class Subscription;  // RAII. Destruction unsubscribes
    using SubscribeCallback =
        std::function<void(std::string_view key, const ParamValue&)>;
    Result<Subscription> Subscribe(std::string_view scope,
                                   std::string_view key_pattern,
                                   SubscribeCallback callback);
};
```

`scope` format: `"base"`, `"session/<sid>"`, `"snap/<sid>"` (read-only).
Put/Delete to `snap` returns `Status::ReadOnly`.

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
    static Result<ParamCache> Open(const Config& config);
    static Result<ParamCache> Open(std::shared_ptr<Transport> transport,
                                   const Config& config);

    /// Join a session: initially fetch snap + overlay, then start delta subscription.
    /// Synchronously blocks until completion (timeout is config.query_timeout).
    Result<void> Attach(std::string_view sid);
    /// Direct base reference mode ([02] §5.3)
    Result<void> AttachBase();
    void Detach();

    // ---- Zero-copy reads [N01] ----
    /// Shared reference to the value. The lock is held only during lookup. Return-value lifetime is managed by shared_ptr.
    std::shared_ptr<const ParamValue> GetShared(std::string_view key) const;

    template<typename T>
    std::optional<T> Get(std::string_view key) const;          // Scalar (copy)
    template<typename T>
    T GetOr(std::string_view key, T default_value) const;

    /// LUT reference: span pointing to the internal buffer + lifetime keepalive handle
    template<typename T>
    struct SpanHandle {
        std::span<const T> span;
        std::shared_ptr<const ParamValue> keepalive;
    };
    template<typename T>
    std::optional<SpanHandle<T>> GetSpan(std::string_view key) const;

    bool Contains(std::string_view key) const;
    /// Enumerate within the cache (no communication)
    void List(std::string_view prefix, const ListSink& sink) const;

    // ---- Writes within a session (put to overlay) ----
    Result<void> Put(std::string_view key, const ParamValue& value);
    Result<void> PutBatch(std::span<const std::pair<std::string, ParamValue>> entries);

    // ---- State ----
    bool stale() const;   // true while disconnected. Recovers by refetching after reconnection [N10]
};
```

### Usage Example (Compute Process)

```cpp
auto cache = sitos::ParamCache::Open(config).value.value();
cache.Attach(sid);

double fov   = cache.GetOr<double>("recon/fov", 240.0);
auto lut     = cache.GetSpan<float>("recon/bhc/lut");   // zero-copy
if (lut) Process(lut->span);

cache.Put("recon/progress", 0.5);   // distributed to all participating processes
```

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
| `ParamCache` | Get methods may run concurrently. Attach/Detach must be serialized externally |
| `StorageNode` | All methods may be called concurrently |
| callback | Called from zenoh threads. Blocking is prohibited. From inside a callback, only Get-style APIs on the same object may be called |

(END OF DOCUMENT)
