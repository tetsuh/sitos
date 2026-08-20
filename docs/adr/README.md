# Architecture Decision Records (ADRs)

This file is the sole comprehensive maintained ADR index for sitos. Every ADR PR must update this
index. See [docs/10_adr_process.md](../10_adr_process.md) for the canonical process.

Each numbered ADR file is authoritative for its own status. This table mirrors the normalized
status so the offline documentation contract can detect index drift.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-use-zenoh-as-the-transport-layer.md) | Use zenoh as the transport layer | Accepted |
| [0002](0002-implement-an-embedded-storage-node.md) | Implement an embedded storage node instead of zenoh storage-manager | Accepted |
| [0003](0003-ship-in-memory-and-rocksdb-engines.md) | Ship InMemory and RocksDB engines; do not adopt LevelDB | Accepted |
| [0004](0004-expose-engine-native-snapshots.md) | Expose engine-native snapshots through the zenoh key space | Accepted |
| [0005](0005-name-the-project-sitos.md) | Name the project sitos | Accepted |
| [0006](0006-cpp20-core-with-python-bindings.md) | C++20 core with Python bindings | Accepted |
| [0007](0007-adopt-legacy-compatible-payload-v1.md) | Adopt legacy-compatible payload v1 with Encoding-based versioning | Accepted |
| [0008](0008-license-under-apache-2.0.md) | License under Apache-2.0 | Accepted |
| [0009](0009-english-as-the-repository-language.md) | English as the repository language | Accepted |
| [0010](0010-use-nanobind-for-python-bindings.md) | Use nanobind for Python bindings | Accepted |
| [0011](0011-develop-in-public-from-day-one.md) | Develop in public from day one | Accepted |
| [0012](0012-google-cpp-style-with-100-column-limit.md) | Google C++ style with 100-column limit | Accepted |
| [0013](0013-default-to-zenoh-scouting-with-explicit-endpoint-override.md) | Default to zenoh scouting with explicit endpoint override | Accepted |
| [0014](0014-session-scoped-buffers.md) | Add a session-scoped, disk-backed buffers key space | Superseded by ADR-0032 |
| [0015](0015-optional-http-gateway-component.md) | Ship an optional HTTP gateway component on cpp-httplib | Superseded by ADR-0027 |
| [0016](0016-use-canonical-zenoh-bytes-encodings.md) | Use canonical zenoh bytes encodings | Accepted |
| [0017](0017-atomic-storage-node-lifecycle.md) | Use atomic, quiescent StorageNode lifecycle transitions | Accepted |
| [0018](0018-use-zenoh-valid-batch-key-segment.md) | Use a zenoh-valid batch key segment | Accepted |
| [0019](0019-client-result-status-configuration.md) | Additive client result and configuration foundation | Accepted |
| [0020](0020-synchronously-complete-transport-get.md) | Synchronously complete Transport Get requests | Accepted |
| [0021](0021-resolve-installed-zenoh-dependency.md) | Resolve installed Zenoh dependencies without fetching or bundling | Accepted |
| [0022](0022-make-param-cache-session-only.md) | Make ParamCache session-only | Accepted |
| [0023](0023-param-cache-consistency-and-lifetime.md) | Define ParamCache consistency and lifetime boundary | Accepted |
| [0024](0024-opt-in-google-benchmark-build-boundary.md) | Keep Google Benchmark opt-in and outside installed packages | Accepted |
| [0025](0025-session-view-lifetime-and-composite-read-consistency.md) | Define SessionView lifetime and composite-read consistency | Accepted |
| [0026](0026-python-wheel-build-and-native-runtime.md) | Define the Python wheel build and bundled native-runtime boundary | Accepted |
| [0027](0027-keep-http-control-planes-in-host-applications.md) | Keep HTTP control planes in host applications | Accepted |
| [0028](0028-unify-acknowledged-operation-results.md) | Unify acknowledged operation results | Accepted |
| [0029](0029-define-same-publisher-fence-ordering.md) | Define same-publisher Fence ordering | Accepted |
| [0030](0030-param-store-subscription-lifetime-and-delivery.md) | Define ParamStore subscription lifetime and delivery semantics | Accepted |
| [0031](0031-cross-platform-vcpkg-foundation.md) | Establish a cross-platform vcpkg foundation | Accepted |
| [0032](0032-mixed-session-buffer-routes.md) | Define mixed durable and ephemeral session buffer routes | Accepted |
| [0033](0033-rocksdb-engine-snapshot-and-package-boundary.md) | Define the RocksDB engine, snapshot, and installed-package boundary | Accepted |
