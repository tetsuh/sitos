# sitos — Issue Breakdown / Milestones and Issue Split Proposal

Split the work into units that other AI models (or developers) can start implementing as-is.
Each Issue has "Reference documents", "Acceptance criteria (AC)", and "Dependencies".

**Issue numbers must match GitHub Issue numbers 1:1** (creation order = the sequence numbers in this document).
Principle for the dependency graph: most of M1 can be implemented in parallel without zenoh dependencies.
zenoh integration (M2) is the overall critical path.

## Release boundaries

Register release boundaries as **GitHub Milestones**, and assign each Issue to the corresponding
Milestone (M0–M5 are design-phase classifications; progress management on GitHub uses
Milestone = release boundary).

| Milestone | Target state | Required Issues |
|---|---|---|
| **v0.1** | The zenoh-independent core works. Payload fixtures and InMemoryEngine contract tests are green | #1, #4, #5, #6, #7 |
| **v0.2** | StorageNode/ParamStore/ParamCache work in C++ through same-process zenoh sessions | #2, #3, #9–#13, #15, #18, #19, #21 |
| **v0.3** | Basic Python APIs work (InMemory, ParamStore, ParamCache, NumPy read) | #22, #23, #24, #25, #27 |
| **v0.4** | RocksDB, single-value interop, bench, and examples are in place | #8, #29, #31, #32, #33 |
| **v1.0** | Public OSS quality. docs/release/wheels/ack/batch interop/GIL/custom engine completed | #14, #16, #17, #20, #26, #28, #30, #34, #35 |

`ack`-related work (#14, #17) is useful, but implementation is heavy relative to initial value,
so it is not a v0.2 blocker. Include it by v1.0.

---

## M0: Repository bootstrap

### #1 Repository skeleton and CI skeleton
* Milestone: v0.1
* References: [06] §1, §2, §5, [00] §6 (D9–D12)
* Scope: CMake project skeleton (option definitions, presets), gtest introduction,
  GitHub Actions `ci.yml` (Windows/Linux build + empty test run),
  `.clang-format` (`BasedOnStyle: Google`, `ColumnLimit: 100`)/.editorconfig,
  `.gitignore`/`.gitattributes` (quote and combine the C++/CMake/Python portions from
  github/gitignore and gitattributes/gitattributes, and clearly note the source URLs in the files),
  `AGENTS.md` (entry point for AI implementers: document pointers + summary of absolute rules.
  Do not duplicate [11]; keep it to pointers),
  LICENSE (Apache-2.0), README skeleton.
  Create `docs/adr/` and write initial ADRs 0001–0013 in English ([10] §8).
  Configure the PR template ([11] §4) and branch protection on main (CI required, direct push prohibited).
  Because the repository is **public from the start** (D11), reserve the `sitos` name on PyPI
  (placeholder release) in this Issue as well. All deliverables are in English (D9)
* Acceptance criteria: CI is green on both OSes. `cmake --preset dev-linux && ctest` passes.
  The `sitos` placeholder exists on PyPI. `docs/adr/` contains 0001–0013
* Depends on: none

### #2 Build integration for zenoh-c/zenoh-cpp
* Milestone: v0.2
* References: [06] §2
* Scope: Prebuilt zenoh-c acquisition + `find_package` / FetchContent integration.
  Pin the version. Smoke test that only opens and closes a zenoh session
* Acceptance criteria: zenoh session open/close test passes in CI on both OSes
* Depends on: #1

### #3 Transport adapter and zenoh compatibility CI
* Milestone: v0.2
* References: [02] §1, [09]
* Implementation targets: `include/sitos/transport.hpp`,
  `src/transport/zenoh_transport.cpp`, `tests/integration/transport_test.cpp`,
  `.github/workflows/dependency-upgrade.yml`
* Scope: Implement a `Transport` abstraction that hides the zenoh-cpp API, so StorageNode/ParamStore/
  ParamCache do not depend directly on raw zenoh-cpp types. Verify the minimum supported and
  latest stable zenoh versions in CI. Confirm that the attachment API is available in the locked version
* Acceptance criteria: transport_test is green for both the minimum supported version and latest stable.
  Static check confirms there are no raw zenoh-cpp API includes outside transport under `src/`
* Depends on: #2

---

## M1: Core (parallelizable without zenoh dependencies)

### #4 ParamValue and payload v1 codec
* Milestone: v0.1
* References: [04] §1, [03] §2
* Implementation targets: `include/sitos/param_value.hpp`, `src/param_value.cpp`,
  `tests/unit/param_value_test.cpp`, `tests/fixtures/payload_v1/*.hex`
* Scope: `ParamValue` (variant, As/AsSpan, Encode/Decode), type tags,
  numeric arithmetic cast rules, encoder/decoder for the batch format ([03] §5).
  Validate the fixtures themselves with an independent implementation (Python struct)
* Acceptance criteria: unit tests including golden tests (byte-for-byte fixture matches).
  Round-trip for all types, boundary values (NaN, ±inf, empty string, empty bytes), rejection of invalid payloads
* Depends on: #1

### #5 Key validation and key-expression construction
* Milestone: v0.1
* References: [03] §1
* Implementation targets: `include/sitos/key.hpp`, `src/key.cpp`, `tests/unit/key_test.cpp`
* Scope: Validate user-key grammar, construct `<prefix>/<scope>/<key>`,
  scope parser ("base" / "session/<sid>" / "snap/<sid>")
* Acceptance criteria: unit tests for valid/invalid cases (reserved characters, empty chunks)
* Depends on: #1

### #6 StorageEngine abstraction and contract test suite
* Milestone: v0.1
* References: [02] §3, [06] §4
* Implementation targets: `include/sitos/storage_engine.hpp`,
  `tests/unit/storage_engine_contract.hpp`, `tests/unit/storage_engine_contract_test.cpp`
* Scope: `StorageReader`/`StorageEngine` interfaces,
  default copy-fallback implementation of `TakeSnapshot()`,
  contract test suite reusable by swapping engine implementations
* Acceptance criteria: Contract tests are written against the abstraction and are green with a mock engine
* Depends on: #1

### #7 InMemoryEngine
* Milestone: v0.1
* References: [02] §3, [04] §3
* Implementation targets: `include/sitos/in_memory_engine.hpp`, `src/in_memory_engine.cpp`,
  `tests/unit/in_memory_engine_test.cpp`
* Scope: `std::map` + `std::shared_mutex` implementation. Snapshots are copies
* Acceptance criteria: Contract tests green. Stress test for concurrent read/write (TSan)
* Depends on: #6

### #8 RocksDBEngine
* Milestone: v0.4
* References: [02] §3, [04] §3, [06] §2
* Implementation targets: `include/sitos/rocksdb_engine.hpp`, `src/rocksdb_engine.cpp`,
  `tests/unit/rocksdb_engine_test.cpp`, `cmake/FindRocksDB.cmake` (if needed)
* Scope: `SITOS_WITH_ROCKSDB` option, `rocksdb::DB` wrapper,
  O(1) TakeSnapshot based on `GetSnapshot()`
* Acceptance criteria: Contract tests green. Put after TakeSnapshot does not affect snapshot reads.
  TakeSnapshot execution time does not depend on data size (bench)
* Depends on: #6

---

## M2: StorageNode and zenoh integration (critical path)

### #9 StorageNode: queryable get/list skeleton
* Milestone: v0.2
* References: [02] §4, [03] §3–4
* Implementation targets: `include/sitos/storage_node.hpp`, `src/storage_node.cpp`,
  `tests/integration/storage_node_query_test.cpp`
* Scope: queryable declaration for `<prefix>/**`, get/List routing to base,
  minimal Stop/RAII implementation
* Acceptance criteria: integration tests — get round-trip from a separate zenoh session,
  chunk-boundary List, 0 replies for get of an unknown key
* Depends on: #3, #4, #5, #7

### #10 StorageNode: subscriber put/delete
* Milestone: v0.2
* References: [02] §4, [03] §3–4
* Implementation targets: `src/storage_node.cpp`,
  `tests/integration/storage_node_subscriber_test.cpp`
* Scope: subscriber declaration for `<prefix>/**`, put/delete routing to base,
  warning logs for read-only paths
* Acceptance criteria: integration tests — put→get round-trip from a separate zenoh session,
  get returns 0 replies after delete, raw put to snap is ignored
* Depends on: #9

### #11 StorageNode lifecycle / RAII
* Milestone: v0.2
* References: [02] §4, [04] §3
* Implementation targets: `include/sitos/storage_node.hpp`, `src/storage_node.cpp`,
  `tests/integration/storage_node_lifecycle_test.cpp`
* Scope: StorageNode::Start/Stop, undeclare queryable/subscriber, move/RAII contract
* Acceptance criteria: queryable/subscriber do not respond after Stop, and double Stop is safe
* Depends on: #10

### #12 StorageNode: session management (snapshot/overlay)
* Milestone: v0.2
* References: [02] §4.1–4.3, §7
* Implementation targets: `include/sitos/session.hpp`, `src/storage_node.cpp`,
  `tests/integration/storage_node_session_test.cpp`
* Scope: CreateSession/CloseSession, snapshot table, overlay table,
  `snap/<sid>/**` get replies, `session/<sid>/**` put/get,
  warnings for read-only violations, `meta/session/<sid>`
* Acceptance criteria: integration tests — snapshot isolation (base put after CreateSession does not affect snap reads),
  get returns 0 replies after Close, resource release (leak test)
* Depends on: #11, (#8 can proceed in parallel)

### #13 Batch delivery
* Milestone: v0.2
* References: [03] §5, [02] §6.1
* Implementation targets: `include/sitos/batch.hpp`, `src/batch.cpp`, `src/storage_node.cpp`,
  `tests/integration/batch_test.cpp`
* Scope: Receiving process for `$batch` keys (`base/$batch`, `session/<sid>/$batch`)
  (atomic apply), and ParamCache receiving batch through the normal subscription range
* Acceptance criteria: integration tests — all batch entries applied and ordered, received through normal subscription,
  expansion into per-key notifications
* Depends on: #11, #12

### #14 ack protocol
* Milestone: v1.0
* References: [03] §6, [02] §6.2
* Implementation targets: `include/sitos/ack.hpp`, `src/ack.cpp`, `src/storage_node.cpp`,
  `tests/integration/ack_test.cpp`
* Scope: ack ring buffer and `meta/ack/<uuid>` queryable, client retry
* Acceptance criteria: integration tests — ack round-trip, retry on ack timeout,
  put without attachment is treated as ack-less
* Depends on: #11

### #15 ParamStore: Put/Get/List/Delete
* Milestone: v0.2
* References: [04] §2
* Implementation targets: `include/sitos/param_store.hpp`, `src/param_store.cpp`,
  `tests/integration/param_store_test.cpp`
* Scope: Open (new/session injection), Put/PutBatch/Delete/Get/Contains/List,
  scope validation, basic Result/Status mapping
* Acceptance criteria: integration tests — all API round-trips against StorageNode.
  Scope validation (Put to snap is ReadOnly)
* Depends on: #11, #13

### #16 ParamStore: Subscribe
* Milestone: v1.0
* References: [04] §2, [01] F13
* Implementation targets: `include/sitos/param_store.hpp`, `src/param_store.cpp`,
  `tests/integration/param_store_subscribe_test.cpp`
* Scope: Subscribe/Subscription RAII, callback error handling, unsubscribe
* Acceptance criteria: integration tests — put notification, expanded batch notifications, no notifications after unsubscribe
* Depends on: #15

### #17 ParamStore: ack/error mapping
* Milestone: v1.0
* References: [03] §6, [04] §2
* Implementation targets: `include/sitos/status.hpp`, `src/param_store.cpp`,
  `tests/integration/param_store_ack_test.cpp`
* Scope: PutOptions::ack, ack timeout/retry, detailed Status mapping
* Acceptance criteria: integration tests — ack success/failure/timeout, Disconnected/Timeout when StorageNode is stopped
* Depends on: #14, #15

---

## M3: ParamCache and SessionView

### #18 ParamCache: initial fetch + delta subscription
* Milestone: v0.2
* References: [02] §5, [04] §4
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`,
  `tests/integration/param_cache_attach_test.cpp`
* Scope: Attach(sid)/AttachBase/Detach, loss-prevention sequence (a)→(b)→(c),
  shared_ptr-based cache, delta application
* Acceptance criteria: integration tests — final state is correct even with a concurrent put during Attach
  (race reproduction test), batch apply, subscription stops after Detach
* Depends on: #5, #12, #13

### #19 ParamCache: zero-copy reads and performance
* Milestone: v0.2
* References: [04] §4, [01] N01
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`,
  `tests/unit/param_cache_read_test.cpp`, `tests/bench/cache_get_bench.cpp`
* Scope: GetShared/Get/GetOr/GetSpan (SpanHandle), List, bench harness
* Acceptance criteria: bench — Get is in the target order (including confirmation of no interprocess communication).
  The old buffer remains valid even if the value is updated while holding SpanHandle (verified with ASan)
* Depends on: #18

### #20 Disconnect/reconnect recovery
* Milestone: v1.0
* References: [02] §9, [01] N10
* Implementation targets: `src/param_cache.cpp`,
  `tests/integration/reconnect_test.cpp`
* Scope: stale flag, zenoh reconnect detection, automatic refetch
* Acceptance criteria: integration — ParamCache recovers across StorageNode restart
* Depends on: #18

### #21 SessionView (host-process facade)
* Milestone: v0.2
* References: [04] §5
* Implementation targets: `include/sitos/session_view.hpp`, `src/session_view.cpp`,
  `tests/unit/session_view_test.cpp`
* Scope: in-process resolving read from overlay → snapshot, Put delivery
* Acceptance criteria: unit + integration — resolution order, consistency with direct engine reference
* Depends on: #12

---

## M4: Python bindings

### #22 nanobind module skeleton and wheel build
* Milestone: v0.3
* References: [05], [06] §3
* Implementation targets: `python/pyproject.toml`, `python/bindings/module.cpp`,
  `python/sitos/__init__.py`, `tests/python/test_import.py`
* Scope: `_sitos` module, scikit-build-core, cibuildwheel configuration,
  ParamValue ↔ Python type conversion
* Acceptance criteria: `pip install dist/*.whl && python -c "import sitos"` on both OSes.
  pytest for type conversion (shared golden fixtures)
* Depends on: #4 (can proceed in parallel; preferably after #15/#18 APIs settle)

### #23 Python ParamStore
* Milestone: v0.3
* References: [05] §2.1
* Implementation targets: `python/bindings/param_store.cpp`, `python/sitos/store.py`,
  `tests/python/test_param_store.py`
* Scope: Python binding for ParamStore, context manager, exception mapping
* Acceptance criteria: pytest — Put/Get/List/Delete/Subscribe round-trip
* Depends on: #15, #16, #22

### #24 Python ParamCache
* Milestone: v0.3
* References: [05] §2.2
* Implementation targets: `python/bindings/param_cache.cpp`, `python/sitos/cache.py`,
  `tests/python/test_param_cache.py`
* Scope: Python binding for ParamCache, attach/detach, stale, items
* Acceptance criteria: pytest — attach/delta-subscription scenarios equivalent to C++ ParamCache
* Depends on: #18, #22

### #25 Python StorageNode / SessionView
* Milestone: v0.3
* References: [05] §2.3–2.4
* Implementation targets: `python/bindings/storage_node.cpp`, `python/bindings/session_view.cpp`,
  `python/sitos/node.py`, `tests/python/test_storage_node.py`
* Scope: Python bindings for StorageNode, InMemoryEngine, SessionView
* Acceptance criteria: pytest — create_session/close_session, session_view overlay→snapshot resolution
* Depends on: #12, #21, #22

### #26 Python callback / GIL dispatch
* Milestone: v1.0
* References: [05] §3
* Implementation targets: `python/bindings/callback_dispatcher.cpp`,
  `tests/python/test_callbacks.py`
* Scope: Python callback dispatch thread, acquire GIL, log exceptions
* Acceptance criteria: pytest — concurrent get inside callback does not deadlock
* Depends on: #23, #24

### #27 NumPy zero-copy and type stubs
* Milestone: v0.3
* References: [05] §1–2, §4
* Implementation targets: `python/bindings/numpy.cpp`, `python/sitos/py.typed`,
  `python/sitos/*.pyi`, `tests/python/test_numpy_zero_copy.py`
* Scope: `get_array(dtype)` (buffer protocol, writeable=False, keepalive),
  ndarray put, `.pyi` stubs
* Acceptance criteria: pytest — verify base-buffer identity (no copy), mypy green
* Depends on: #24

### #28 Python custom engine
* Milestone: v1.0
* References: [05] §2.3, [01] X01
* Implementation targets: `python/bindings/storage_engine_trampoline.cpp`,
  `tests/python/test_custom_engine.py`
* Scope: Support Python inheritance of `sitos.StorageEngine` (trampoline)
* Acceptance criteria: Python engine passes the contract tests (pytest version)
* Depends on: #25

---

## M5: Interoperability, documentation, and release

### #29 zenoh-python single-value interoperability test
* Milestone: v0.4
* References: [03], [01] C03, [06] §4
* Implementation targets: `tests/interop/test_raw_zenoh_single.py`
* Scope: Interop test that put/get/subscribe single values using only zenoh-python + struct,
  without using the sitos library
* Acceptance criteria: Test written only from the wire specification is green
* Depends on: #11, #12

### #30 zenoh-python batch/ack interoperability test
* Milestone: v1.0
* References: [03] §5–6, [01] C03, [06] §4
* Implementation targets: `tests/interop/test_raw_zenoh_batch_ack.py`
* Scope: Verify `$batch` and ack using only zenoh-python + struct
* Acceptance criteria: batch/ack test written only from the wire specification is green
* Depends on: #13, #14

### #31 C++ examples and demo binary sitobolon
* Milestone: v0.4
* References: [00] §4, [06] §1
* Implementation targets: `examples/cpp/quickstart.cpp`, `examples/cpp/sitobolon.cpp`,
  `examples/cpp/CMakeLists.txt`
* Scope: C++ quickstart, standalone StorageNode executable
  `sitobolon` (engine selected by config file)
* Acceptance criteria: C++ examples are built and run in CI
* Depends on: #15, #18

### #32 Python examples
* Milestone: v0.4
* References: [05], [06] §1
* Implementation targets: `examples/python/quickstart.py`, `examples/python/numpy_lut.py`,
  `examples/python/raw_zenoh.py`
* Scope: Python quickstart, NumPy LUT sample, zenoh-python raw interop sample
* Acceptance criteria: Python examples are run in CI
* Depends on: #23, #24, #27, #29

### #33 Benchmark CI and performance requirement verification
* Milestone: v0.4
* References: [01] N01–N02, N08–N09, [06] §4–5
* Implementation targets: `tests/bench/*.cpp`, `.github/workflows/bench.yml`
* Scope: bench workflow, baseline management, N08/N09 measurement scenarios
* Acceptance criteria: nightly bench runs and produces comparison reports against requirement values
* Depends on: #8, #18, #19

### #34 Documentation site
* Milestone: v1.0
* References: [06] §5, §7
* Implementation targets: `docs/`, `Doxyfile`, `docs/conf.py`, `.github/workflows/docs.yml`
* Scope: Doxygen/Sphinx, completed README, CONTRIBUTING, docs.yml
* Acceptance criteria: docs.yml green. Public documents do not reference private documents
* Depends on: #1

### #35 Release / publication readiness
* Milestone: v1.0
* References: [06] §7
* Implementation targets: `NOTICE`, `.github/workflows/wheels.yml`,
  `.github/release-please-config.json`, `pyproject.toml`
* Scope: NOTICE, complete the pre-publication checklist, reserve PyPI/crates names, release-please
* Acceptance criteria: All checklist items completed, dry-run publish to TestPyPI succeeds
* Depends on: M4 completed, #34

---

## Recommended implementation order (parallel lanes)

```
Lane A (core):      #1 → #4 → #6 → #7 → #8
Lane B (zenoh):     #2 → #3 → #9 → #10 → #11 → #12 → #18 → #19/#20
                                            └→ #13 → #15/#16
                                                #14 → #17
Lane C (Python):    #22 (any time after #4) → #23/#24/#25 → #26 → #27/#28
Lane D (quality):   #29/#30, #31/#32, #33, #34/#35 (as dependencies complete)
```

Each Issue is assumed to correspond to one PR. The PR must describe the corresponding requirement IDs
and the AC verification results.

Legacy API compatibility layers and migration work for specific products are proprietary-side work,
so they are not included in this Issue list.

(END OF DOCUMENT)
