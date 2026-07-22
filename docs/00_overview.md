# sitos — Overview

> **sitos** (σῖτος, Greek for “grain” or “food”) —
> A distributed parameter store for compute pipelines, powered by Eclipse zenoh.

## 1. Vision

A public OSS library for both C++ and Python that supplies parameters and LUTs
(Look-Up Tables) to each compute process in distributed compute systems
(image reconstruction, signal processing, simulation, and similar workloads)
**without timing bugs and with low overhead**.

sitos generalizes requirements obtained from existing parameter store
implementations for distributed computation and publishes them in an independent
repository as a general-purpose library not limited to CT reconstruction.

## 2. Elevator Pitch

* **Problem**: In distributed computation, managing “which process sees which
  version of which parameter, and when” is difficult, and custom synchronization
  code becomes a breeding ground for timing bugs.
* **Solution**: sitos builds a typed key-value store on top of the zenoh key
  space. At compute session startup, a **snapshot** isolates the session from
  external input, and changes during execution are delivered to every process
  through **overlay + pub/sub differential distribution**. The read hot path is
  a **zero-copy reference to an in-process cache**.
* **Differentiators**:
  - Pluggable storage engines (InMemory / RocksDB / user-defined additions)
  - Exposes native engine snapshots in the zenoh key space in O(1)
  - zenoh wire compatibility: languages with zenoh bindings can read and write
    without sitos bindings

## 3. Scope

### In scope

* Typed key-value store (bool / int64 / double / string / bytes)
* Storage of LUTs (large byte strings and numeric arrays) and zero-copy reads
* Per-session (per-job) snapshots and overlays
* Inter-process sharing through zenoh get/put/subscribe
* Pluggable storage engine abstraction
* C++20 core + Python bindings (NumPy integration)

### Out of scope

* Transfer of image data or compute artifacts themselves (sitos targets
  parameters/LUTs)
* Transactions (atomic updates of multiple keys are limited to the scope of a
  Put batch)
* Access control and authentication (use zenoh ACL mechanisms if needed)
* Conflict resolution for multi-master writes (consistency beyond
  last-write-wins)

## 4. Glossary

| Term | Meaning |
|---|---|
| **StorageEngine** | Abstraction for persistence backends: put/get/list/delete + optional snapshot |
| **StorageNode** | Component that connects zenoh queryables/subscribers with a StorageEngine. The “storehouse” of the sitos key space. Nickname: *sitobolon* (σιτοβολών = granary) |
| **base** | Master data: externally supplied parameters, LUTs, and default values |
| **session** | Unit of compute execution. The lifetime unit for snapshots and overlays |
| **snapshot** | A consistent read view of base at session startup. It is isolated from subsequent base updates |
| **overlay** | Differential area that receives Puts during session execution. Reads resolve in overlay → snapshot order |
| **SessionView** | The logical KV view seen from a session, combining overlay + snapshot |
| **ParamCache** | Read cache inside a subscriber-side process. Provides zero-copy reads |
| **payload v1** | Encoding format consisting of a 1-byte type tag + little-endian raw value (§[03](03_wire_protocol.md)) |

## 5. Document Map

| Document | Visibility | Contents |
|---|---|---|
| [00_overview.md](00_overview.md) | Public | This document. Vision, scope, and terminology |
| [01_requirements.md](01_requirements.md) | Public | Requirements specification (with IDs) |
| [02_architecture.md](02_architecture.md) | Public | Architecture specification (components, sequences, thread model) |
| [03_wire_protocol.md](03_wire_protocol.md) | Public | Wire protocol specification (key space, encoding, query semantics) |
| [04_api_cpp.md](04_api_cpp.md) | Public | C++ API specification |
| [05_api_python.md](05_api_python.md) | Public | Python API specification |
| [06_build_test_packaging.md](06_build_test_packaging.md) | Public | Build, testing, and packaging |
| [07_issue_breakdown.md](07_issue_breakdown.md) | Public | Milestones and proposed issue breakdown |
| [08_contract_registry.md](08_contract_registry.md) | Public | Public contract registry: index of wire surfaces and stable identifiers |
| [09_dependency_policy.md](09_dependency_policy.md) | Public | Dependency and zenoh compatibility policy |
| [10_adr_process.md](10_adr_process.md) | Public | ADR writing and operation rules (to be moved to docs/adr/README.md) |
| [development_workflow.md](development_workflow.md) | Public | Branching strategy, TiDD, and TDD operation rules (to be moved to CONTRIBUTING) |

Public documents (00–08) are maintained under `docs/` in the sitos repository.
Public documents must not contain references to non-public documents.

## 6. Record of Major Decisions (ADR Summary)

The operation rules are in [10_adr_process.md](10_adr_process.md). At repository
creation, D1 through D13 are filed in English as individual ADRs under
`docs/adr/0001` through `0013`; after that, this table is maintained as an
index to the ADRs.

| # | Decision | Rationale |
|---|---|---|
| D1 | Adopt zenoh as the communication foundation | Integration of pub/sub + query + storage, elimination of custom synchronization code, distributed extensibility |
| D2 | Implement our own StorageNode in C++ instead of using zenoh storage-manager | The zenoh storage trait does not expose engine snapshots. Avoid requiring Rust plugin development to add engines |
| D3 | Engines are InMemory (default) + RocksDB (opt-in). Do not adopt LevelDB | LevelDB is frozen upstream (stagnant since v1.23/2024). Unsuitable as a bundled dependency for public OSS |
| D4 | Snapshots use engine-native functionality + queryable exposure | Same semantics as the current LevelDB snapshot in O(1). They can be inspected from zenoh without copying |
| D5 | Name the project sitos | A coined term derived from σῖτος (grain/food). It represents “provisioning food for distributed computation” and is unused in major registries (PyPI/crates.io) |
| D6 | C++20 core + Python bindings (Holoscan style) | No boost dependency. Uses `std::span` as a standard feature. One-shot pip install. NumPy zero-copy |
| D7 | payload v1 follows the legacy-compatible format | Easy migration from existing systems. Versioned by Encoding schema name, allowing migration to CBOR or similar formats in the future |
| D8 | License is Apache-2.0 | Compatible with zenoh (EPL-2.0/Apache-2.0) and RocksDB (Apache-2.0/GPLv2) |
| D9 | The repository language is English (code, comments, docs, issues) | The standard for international OSS. This design document set is maintained in English in docs/ |
| D10 | Python bindings use nanobind | Lightweight and fast. buffer protocol / GIL control matches requirements (P02, P04) |
| D11 | The repository is public from the beginning | The development process is public as well. Operational discipline to keep internal information out of commit history is enforced from the start |
| D12 | Code style is Google-based + ColumnLimit 100 | Design document naming (PascalCase methods, trailing `_` members) conforms to Google style. It also has the best affinity with implementation AI |
| D13 | zenoh connection defaults to scouting + explicit endpoint configuration through Config | Balances a zero-configuration development experience with support for environments where multicast is prohibited (such as medical devices) |

(END OF DOCUMENT)
