# Architecture Decision Records (ADRs)

This directory contains the ADRs for sitos.
See [docs/10_adr_process.md](../10_adr_process.md) for the process.

| ADR | Title |
|-----|-------|
| [0001](0001-use-zenoh-as-the-transport-layer.md) | Use zenoh as the transport layer |
| [0002](0002-implement-an-embedded-storage-node.md) | Implement an embedded storage node instead of zenoh storage-manager |
| [0003](0003-ship-in-memory-and-rocksdb-engines.md) | Ship InMemory and RocksDB engines; do not adopt LevelDB |
| [0004](0004-expose-engine-native-snapshots.md) | Expose engine-native snapshots through the zenoh key space |
| [0005](0005-name-the-project-sitos.md) | Name the project sitos |
| [0006](0006-cpp20-core-with-python-bindings.md) | C++20 core with Python bindings |
| [0007](0007-adopt-legacy-compatible-payload-v1.md) | Adopt legacy-compatible payload v1 with Encoding-based versioning |
| [0008](0008-license-under-apache-2.0.md) | License under Apache-2.0 |
| [0009](0009-english-as-the-repository-language.md) | English as the repository language |
| [0010](0010-use-nanobind-for-python-bindings.md) | Use nanobind for Python bindings |
| [0011](0011-develop-in-public-from-day-one.md) | Develop in public from day one |
| [0012](0012-google-cpp-style-with-100-column-limit.md) | Google C++ style with 100-column limit |
| [0013](0013-default-to-zenoh-scouting-with-explicit-endpoint-override.md) | Default to zenoh scouting with explicit endpoint override |
| [0014](0014-session-scoped-buffers.md) | Add a session-scoped, disk-backed buffers key space |
| [0015](0015-optional-http-gateway-component.md) | Ship an optional HTTP gateway component on cpp-httplib |
| [0016](0016-use-canonical-zenoh-bytes-encodings.md) | Use canonical zenoh bytes encodings |
| [0017](0017-atomic-storage-node-lifecycle.md) | Use atomic, quiescent StorageNode lifecycle transitions |
| [0018](0018-use-zenoh-valid-batch-key-segment.md) | Use a zenoh-valid batch key segment |
| [0019](0019-client-result-status-configuration.md) | Additive client result and configuration foundation |
| [0020](0020-synchronously-complete-transport-get.md) | Synchronously complete Transport Get requests |
