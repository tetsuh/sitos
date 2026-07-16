# sitos — Dependency / Zenoh Compatibility Policy

## 1. Basic policy

sitos **owns its wire specification and thinly isolates zenoh as a transport dependency**.

### 1.1 Backend-isolated logging

The core logging API has no mandatory third-party backend dependency. Applications may provide
plog, quill, or another backend through a `LogSink` adapter; backend-specific types, macros,
and initialization never enter sitos component APIs. `LogRecord` string views are callback-scoped,
so asynchronous adapters must copy the component and message before `Write()` returns.

`LogSink::Write()` may be called concurrently, so sink implementations must synchronize access
to their mutable state. `EmitLog()` is the non-throwing exception-containment boundary, while
backend lifecycle, configuration, filtering, formatting, and ownership remain with the
application or optional adapter. An explicitly null sink disables emission; omitted `StorageNodeConfig` sinks use the
immutable built-in stderr sink.

* The compatibility units for sitos are the `sitos.v1` payload, key space, and batch/ack protocols,
  not zenoh internal APIs
* The scope affected by zenoh version upgrades is limited to `src/transport/zenoh_transport.cpp`
  and CI configuration
* The semantics of `ParamValue` / `StorageEngine` / `StorageNode` / `ParamStore` / `ParamCache`
  do not change when the zenoh version changes

## 2. Supported versions

Initial policy:

```
zenoh-c >= 1.9.0, < 2.0
```

* sitos v1.x supports zenoh-c 1.9.0 and later compatible 1.x releases
* If zenoh-c 2.x support breaks wire/API compatibility, treat it as sitos v2.0
* After validation by `dependency-upgrade.yml`, zenoh-c patch/minor updates are incorporated into
  sitos patch/minor releases

## 3. Transport adapter

Only the transport adapter uses the zenoh-c API directly. `OpenZenohTransport` accepts an
optional complete JSON5 configuration and returns `Result<std::unique_ptr<Transport>>`;
`nullopt` selects `z_config_default()`, an empty or malformed configuration is
`Status::InvalidArgument`, and parse-success/session-open failures retain their native
cause. `MakeZenohTransport()` remains the compatibility wrapper that converts any failure
to `nullptr`. Configuration text is never retained or included in diagnostics.

Client-facing status classification and `ClientConfig` validation are dependency-free and
live in `status.hpp`, `result.hpp`, and `client_config.hpp`. The adapter explicitly classifies
known disconnected and invalid-argument conditions while retaining their native Zenoh causes
through `Result::Error()`. Type-changing internal propagation uses `Result::ErrFrom` to retain
Status, message, and cause; native Zenoh failures without a reliable semantic classification
remain `Status::Error`.

Higher-level components see only the following abstract API.

```cpp
namespace sitos {

struct TransportSample {
    std::string key;
    std::span<const std::byte> payload;
    Encoding encoding;
    std::optional<std::string> ack_token;
    enum class Kind { Put, Delete } kind;
};

struct TransportQuery {
    std::string keyexpr;
    Result<void> Reply(std::string_view key,
               std::span<const std::byte> payload,
               Encoding encoding);
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual Result<void> Put(std::string_view key,
                             std::span<const std::byte> payload,
                             Encoding encoding,
                             PutOptions options) = 0;
    virtual Result<void> Delete(std::string_view key, PutOptions options) = 0;

    using QueryResultSink =
        std::function<bool(std::string_view key,
                           std::span<const std::byte> payload,
                           Encoding encoding)>;
    virtual Result<void> Get(std::string_view keyexpr,
                             const QueryResultSink& sink,
                             std::chrono::milliseconds timeout) = 0;

    virtual Result<Subscription> DeclareSubscriber(
        std::string_view keyexpr,
        std::function<void(const TransportSample&)> callback) = 0;

    virtual Result<Queryable> DeclareQueryable(
        std::string_view keyexpr,
        std::function<void(TransportQuery&)> callback) = 0;
};

} // namespace sitos
```

This abstraction is limited to **only the zenoh features that sitos needs**:

| Feature | Purpose |
|---|---|
| put/delete | Writes to base/session |
| get/queryable/reply | Reads, List, snapshot exposure, ack confirmation |
| subscriber | Delta delivery, ParamCache updates |
| Encoding | Identification of `sitos.v1` / `sitos.v1.batch` |
| attachment | ack token (treated as ack-less put in unsupported environments) |

Do not depend directly on advanced APIs, unstable APIs, routing policies, and similar features.

## 4. CI policy

### 4.1 Normal CI

`ci.yml` runs with the locked default zenoh version.
This version is fixed per release branch.

### 4.2 dependency-upgrade CI

`dependency-upgrade.yml` runs the following on nightly / manual triggers:

1. **minimum supported**: the pinned `ZENOHC_VERSION` in `cmake/zenohc.cmake`
2. **latest stable**: the latest stable release at execution time

Validation targets:

* C++ build
* unit / integration / interop
* wire fixtures (`PayloadV1GoldenFixtures`, `BatchV1GoldenFixture`)
* `RawZenohClientCanPutAndGet`
* `RawZenohClientCanSendBatch`

If latest stable fails:

* Do not break the main branch (keep using the locked version)
* Create an Issue labeled `dependency: zenoh-upgrade` automatically or manually
* In principle, limit fixes to `src/transport/zenoh_transport.cpp` and build/CI configuration

## 5. Update procedure

1. Check the zenoh release notes and evaluate whether breaking changes affect the transport adapter
2. Run `dependency-upgrade.yml` manually
3. If failures can be absorbed inside the transport adapter:
   - fix the adapter
   - confirm CI green for both minimum/latest
   - update the locked version
   - make a sitos patch/minor release
4. If wire/API compatibility is affected:
   - create an ADR for sitos v2
   - design the `sitos.v2` schema and v1/v2 dual support during the migration period

## 6. Invariants that preserve compatibility

The following must not change after zenoh updates:

* Byte sequences of the `sitos.v1` payload fixtures
* key paths (`base`, `session`, `snap`, `:batch`, `meta`)
* The loss-prevention sequence of `ParamCache::Attach`
* Snapshot isolation semantics
* Python API exception mapping

## 7. RocksDB / Python dependencies

As with zenoh, monitor RocksDB and Python packaging with locked versions + latest CI.
However, RocksDB is optional (`sitos-rocksdb`) and must not impair availability of the standard
wheel.

(END OF DOCUMENT)
