# sitos — Requirements

Requirement ID format: `sitos-req-<category><number>`
Categories: F=functional, N=non-functional, C=compatibility, P=Python, X=extensibility

Priority: **MUST** (required for the initial release) / **SHOULD** (target for
the initial release) / **MAY** (can be addressed in the future)

The “Inherited from” column shows the corresponding requirements inherited from
the preceding legacy parameter store implementation.

## 1. Functional Requirements (F)

| ID | Priority | Requirement | Inherited from |
|---|---|---|---|
| F01 | MUST | Provide a key-value store whose keys are hierarchical strings and whose values are typed (BOOL/S64/DP/STR/BYTES) | R02 |
| F02 | MUST | Scalar parameters and LUTs (byte strings and numeric arrays) can be read and written through the same API | R02 |
| F03 | MUST | Keys can be enumerated by key prefix (List) | — |
| F04 | MUST | Multiple processes can read and write the same key space. Writes are automatically distributed to all subscriber processes | R06 |
| F05 | MUST | At session (compute execution unit) startup, a consistent snapshot of base can be taken and is isolated from subsequent base updates | R04 |
| F06 | MUST | Parameters can be added and changed during session execution (overlay), and changes are distributed to all participating processes | R05 |
| F07 | MUST | Provide SessionView (a composite view that resolves in overlay → snapshot order) | R04, R05 |
| F08 | MUST | The contents of snapshots and overlays can be read through the zenoh key space (for debugging and external tools) | R07 |
| F09 | MUST | Multiple keys can be Put as a batch (applied as one message on the receiver side) | — |
| F10 | MUST | snapshot / overlay resources are released when the session ends | — |
| F11 | SHOULD | Key existence can be checked (Contains) | — |
| F12 | SHOULD | Keys can be deleted (Delete) (base only; snapshots are immutable) | — |
| F13 | SHOULD | Value-change subscriptions (subscribe callbacks specified by key pattern) are exposed to applications | — |
| F14 | MAY | TTL, history, and generation management | — |

## 2. Non-Functional Requirements (N)

| ID | Priority | Requirement |
|---|---|---|
| N01 | MUST | The subscriber-side read hot path completes using only an in-process cache reference and involves no copies, system calls, or inter-process communication. Guideline: on the order of several hundred ns per Get |
| N02 | MUST | Snapshot acquisition is O(1) when the engine supports it (does not copy in proportion to data volume) |
| N03 | MUST | Engines without snapshot support operate with the same semantics (copy-based fallback) |
| N04 | MUST | The system operates in a single-machine configuration without an external daemon (zenohd) (peer connection) |
| N05 | MUST | The system operates on Windows (MSVC) / Linux (gcc, clang) |
| N06 | MUST | Core library dependencies are limited to the C++20 standard library + zenoh-cpp/zenoh-c (boost is prohibited. RocksDB is opt-in) |
| N07 | MUST | Thread-safe: concurrent Get from multiple threads and concurrent execution of Get and Put are safe |
| N08 | SHOULD | Session startup (snapshot acquisition + initial fetch assuming 10k keys / 100 MB LUT) completes within 1 second |
| N09 | SHOULD | Latency from Put to reflection in another process's cache is within 10 ms on the same host (median) |
| N10 | SHOULD | A crashed subscriber process can recover after restart through reconnection and refetch |
| N11 | MAY | The system operates in a multi-machine configuration (through a zenoh router). This is achieved only through transport configuration with no API changes |

## 3. Compatibility Requirements (C)

| ID | Priority | Requirement |
|---|---|---|
| C01 | MUST | payload v1 (1-byte type tag + LE raw value) is the wire and storage format and is strictly defined in [03_wire_protocol.md](03_wire_protocol.md) |
| C02 | MUST | A format identifier is set in zenoh `Encoding`, making coexistence with future payload v2 (CBOR, etc.) possible |
| C03 | MUST | Reading and writing are possible using only standard zenoh APIs (get/put/subscribe) — interoperability with clients that do not depend on the sitos library (such as zenoh-python) |
| C04 | MUST | Semantic versioning is adopted, and wire compatibility is broken only in major versions |
| C05 | MUST | Provide an API that allows external compatibility adapters (such as wrappers for legacy KV APIs) to be implemented without changing the sitos core. In particular, Get with arithmetic casts between numeric types, prefix enumeration, and batch Put semantics must be available from the public API |

> **Note**: Compatibility layers for product-specific legacy APIs are outside
> the scope of sitos (public OSS). C05 specifies only the responsibility on the
> sitos side: making it possible to implement external adapters.

## 4. Python Requirements (P)

| ID | Priority | Requirement |
|---|---|---|
| P01 | MUST | Provide a Python API with the same concepts and semantics as C++ (ParamStore/ParamCache/SessionView) |
| P02 | MUST | LUTs can be read zero-copy as NumPy arrays (buffer protocol) |
| P03 | MUST | Binary wheels are installed by `pip install sitos`, with no Rust/C++ toolchain required (Windows/Linux x86_64) |
| P04 | MUST | subscribe callbacks can be registered as Python functions and do not cause GIL deadlocks |
| P05 | SHOULD | Support Python 3.10+ |
| P06 | MAY | asyncio integration (async subscribe iterator) |

## 5. Extensibility Requirements (X)

| ID | Priority | Requirement |
|---|---|---|
| X01 | MUST | Through the StorageEngine abstraction, users can add new persistence backends without changing the library core |
| X02 | MUST | Include InMemoryEngine (zero dependencies) and RocksDBEngine |
| X03 | MUST | The key prefix (default `sitos`) can be changed by configuration (coexistence of multiple instances on the same zenoh network) |
| X04 | SHOULD | A zenoh session can be injected from outside (sharing a session owned by the application) |
| X05 | MAY | Pluggable payload codecs (registration of formats other than v1) |

## 6. Common Principles for Acceptance Criteria

* Each MUST requirement must be verified by at least one automated test
  ([06_build_test_packaging.md](06_build_test_packaging.md) §Test Strategy)
* N01/N02/N08/N09 must be measured by a benchmark harness and regressions must
  be detected in CI
* C03 must be verified by an “interoperability test using only zenoh-python”

(END OF DOCUMENT)
