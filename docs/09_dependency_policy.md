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

### 2.1 Installed CMake packages

An installed Zenoh-OFF `sitos` package has no Zenoh dependency. An installed Zenoh-ON package
uses its installed `Findzenohc.cmake` module to recreate or reuse the externally provisioned
`zenohc::zenohc` target before loading the exported static `sitos::sitos` target. Package
discovery never uses FetchContent or network access. Consumers provide zenoh-c through
`zenohc_ROOT`, `ZENOHC_ROOT`, or a normal CMake prefix when it is not installed in a standard
location. The installed find module cannot enforce the supported zenoh-c version range because
standalone releases do not expose portable installed version metadata; dependency selection is
the consumer's responsibility and the range is validated by the dependency-upgrade workflow.

The sitos install tree does not bundle zenoh-c. The application or package manager is
responsible for deploying the shared `zenohc.dll`/`libzenohc.so` runtime and configuring the
platform runtime search path. See ADR-0021.

## 3. Transport adapter

Only the transport adapter uses the zenoh-c API directly. `OpenZenohTransport` accepts an
optional complete JSON5 configuration and returns `Result<std::unique_ptr<Transport>>`;
`nullopt` selects `z_config_default()`, an empty or malformed configuration is
`Status::InvalidArgument`, and parse-success/session-open failures retain their native
cause. `MakeZenohTransport()` remains the compatibility wrapper that converts any failure
to `nullptr`. Configuration text is never retained or included in diagnostics.

Client-facing status classification and `ClientConfig` validation are dependency-free and
live in `status.hpp`, `result.hpp`, and `client_config.hpp`. The adapter creates synthesized
`sitos.transport` causes for disconnected-session, invalid-operation-argument, and dead-query
failures; callers observe these causes through `Result::Error()`. Pure input-validation failures
may instead carry the corresponding `sitos.status` cause. Native Zenoh failures retain
`sitos.zenoh` causes. Distinct strongly typed construction paths and error categories prevent
equal numeric values from crossing these diagnostic namespaces. Type-changing internal
propagation uses `Result::ErrFrom` to retain Status, message, and cause; native Zenoh failures
without a reliable semantic classification remain `Status::Error`.

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

`Get` requires a strictly positive timeout and returns only after terminal
reply-closure completion and callback quiescence. Terminal zero replies are
successful transport completion. Sinks are serialized within one request and
are never invoked after Get returns; a false result suppresses later delivery
but does not bypass the completion wait. Distinct requests may invoke sinks
concurrently. Low-level Get sinks must not recursively call blocking Get on the
same Transport, but may call nonblocking Put or Delete. An `Error` result does
not imply the sink was never invoked: a reply-processing failure can follow the
delivery of earlier concrete keys. See ADR-0020.

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
* `RawZenohClientCanUseMixedSessionBuffers`
* `RawZenohDurableLateJoinPreservesDistinctKeys`
* `RawZenohBufferInteropFixtureBoundaries`

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
* Buffer key paths are exactly `buffers/<sid>/durable/<key>` and
  `buffers/<sid>/ephemeral/<key>`; `BufferClass` is reserved only in the route-class position.
* Buffer values use opaque bare `zenoh/bytes`; parameter values retain payload-v1 encoding.
* Durable buffer values are retrievable by Get/List, ephemeral values are live-only, and no
  ephemeral state is retained by StorageNode.
* Durable late join observes materialized Get replies, then buffered samples, then post-transition
  live samples, with only documented same-byte duplicate handling.

These buffer invariants are owned by Accepted ADR-0032 and are validated across stages #139, #141,
and final #56; they add no dependency and do not change the transport adapter boundary.

## 7. RocksDB / Python dependencies

As with zenoh, monitor RocksDB and Python packaging with locked versions + latest CI.
However, RocksDB is optional and must not impair availability of the standard wheel.

Issue #35 publishes only the RocksDB-OFF Linux CPython 3.12 standard wheel. Windows remains a
non-publishing validation target. No `sitos-rocksdb` package or Python extra is defined: the public
Python API currently exposes only `InMemoryEngine`, so a RocksDB wheel would provide no supported
selection path. A separate wheel is reconsidered only with an approved public Python RocksDB API or
concrete persistence/performance requirement. C++ consumers retain the opt-in vcpkg route below.

### 7.1 Optional vcpkg foundation

The root `vcpkg.json` is the sole source of the vcpkg executable and built-in registry pin. Its
non-default `rocksdb` feature is validated through the official detached checkout at
`40f3c709db80acf154ac4b17a1f83c564ebd022e`, resolving RocksDB 11.1.2 port-version 0 with the
port's default zlib feature. Canonical CI uses only built-in `x64-linux` and `x64-windows`
triplets and explicit isolated install roots.

Provisioning is opt-in and explicit: project CMake and installed package configuration do not
clone, bootstrap, install, update, or otherwise invoke a package manager, and package discovery
has no hidden network behavior. Existing scoped Zenoh and Google Benchmark `FetchContent` paths
remain unchanged. Standard builds, package validators, and wheels remain RocksDB-OFF and do not
use vcpkg.

Dedicated vcpkg jobs persist a per-platform package-ABI archive directory through a SHA-pinned
GitHub-owned `actions/cache` action and configure vcpkg with
`clear;files,<directory>,readwrite`. Primary keys separate cache format, OS, architecture, canonical
triplet, root manifest hash, and GitHub-hosted runner image identity. A compatible-prefix restore
may seed a new image key, but only an exact primary-key hit skips save. Pull-request writes remain
isolated to their merge-ref scope; trusted pushes to `main` use their normal cache scope. Save occurs
only after the complete validator succeeds, and no secret, long-lived credential, NuGet feed, Mono
runtime, or external cache service is used. Cache behavior is performance-only and cannot make
provisioning or runtime validation optional. A baseline or port version change requires an explicit
dependency-update PR and cold/cached validation on both canonical triplets. Homebrew and macOS
provisioning are unsupported; `apt` is limited to Linux CI bootstrap prerequisites. On Windows, the
canonical static `RocksDB::rocksdb` target imports the baseline's zlib 1.3.2 runtime as `z.dll`;
validation stages that file under its actual name, requires `dumpbin` from the `vcvars64.bat`
environment, and rejects `zlib1.dll` compatibility copies and `rocksdb-shared.dll` dependencies.
Historical backend failures and canonical cold/cached measurements are recorded in ADR-0031
rather than duplicated in this policy document. RocksDBEngine package reconstruction and snapshot
lifetime are defined by ADR-0033. Installed RocksDB-ON consumers are configure/build/link-only and do
not execute or stage runtime dependencies; runtime is covered by the RocksDB engine tests and the
vcpkg clean-stage probe. Exact version equality is necessary but not sufficient for ABI compatibility,
so external consumers own compiler, CRT, standard-library ABI, build configuration, and compile-option
compatibility.

(END OF DOCUMENT)
