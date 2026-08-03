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
| **v0.3** | Basic Python APIs work (InMemory, ParamStore, ParamCache, NumPy read) | #16, #22, #23, #24, #25, #27 |
| **v0.4** | Cross-platform vcpkg foundation, RocksDB engine, process-isolated raw-Zenoh interop, session-scoped buffers, benchmarks, and C++/Python examples are in place | #122, #8, #29, #31, #32, #33, #139, #140, #141, #56 |
| **v0.5** | Reliable and durable session-buffer delivery is ready for downstream application integration | #14, #17, #99, #105–#109 |
| **v1.0** | Public OSS quality, reconnect recovery, advanced Python extensions, raw batch/ack interop, documentation, and publication readiness are complete | #20, #26, #28, #30, #34, #35 |

`ack`-related work (#14, #17) is useful, but implementation is heavy relative to initial value,
so it is not a v0.2 blocker. Include it by v0.5.

For v0.4, #122 precedes #8; #8 precedes the RocksDB-dependent #31, #33, and #56; #29 precedes
raw-Zenoh consumers such as #32 and #56. The accepted ADR-0032 implementation sequence is
#139 (key contract), #140 (SessionRecord lifecycle), #141 (capabilities and routing), then #56
(final raw-Zenoh, RocksDB, package, and combined-CI integration).

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

### #105 StorageEngine durability barrier
* Milestone: v0.5
* References: [02] §3, [04] §3, [10] §6
* Implementation targets: `include/sitos/storage_engine.hpp`, InMemory/RocksDB engine sources,
  reusable contract tests, and RocksDB integration tests
* Scope: define an explicit storage synchronization boundary for writes completed before its
  linearization point; keep ordinary Put operations unsynchronized
* Acceptance criteria: InMemory no-op, RocksDB durability and injected-failure tests, concurrent
  linearization coverage, and clear applied/persisted/synchronized documentation
* Depends on: #8; requires an Accepted ADR before merge

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
* Scope: StorageNode receives and applies `:batch` keys (`base/:batch`,
  `session/<sid>/:batch`) after full validation, with node-local subscriber ordering;
  prove the original batch sample is received through the normal subscription range.
  ParamCache expansion into per-key notifications is deferred to Issue #16.
* Acceptance criteria: integration tests — all batch entries applied and ordered,
  received as one original sample through the normal subscription range
* Depends on: #11, #12

### #14 ack protocol
* Milestone: v0.5
* References: [03] §6, [02] §6.2
* Implementation targets: `include/sitos/ack.hpp`, `src/ack.cpp`, `src/storage_node.cpp`,
  `tests/integration/ack_test.cpp`
* Scope: ack ring buffer and `meta/ack/<uuid>` queryable plus one-submit/multiple-poll helper;
  implementation is blocked until token attachment, batch outcome, and ambiguous Timeout are defined
* Acceptance criteria: integration tests — ack round-trip, query-only retry, ring eviction, malformed
  token rejection, and ack-less compatibility
* Depends on: #11, #85, #86

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
* Milestone: v0.3
* References: [04] §2.1, [01] F13, ADR-0030
* Implementation targets: `include/sitos/param_subscription.hpp`, `include/sitos/param_store.hpp`,
  `src/param_subscription.cpp`, `tests/unit/param_store_subscribe_test.cpp`,
  `tests/integration/param_store_subscribe_test.cpp`
* Scope: delta-only PUT/DELETE subscription, staging, ordered `:batch` expansion, callback exception
  containment, injected client logging, synchronous callback-quiescent RAII close
* Acceptance criteria: deterministic fake-Transport tests for validation/staging/ordering/re-entry/
  Close and one same-session Zenoh integration test for PUT/DELETE/batch/control-subscriber delivery
* Depends on: #13, #15, #76, #85, #88, #90; ADR-0030 must be Accepted immediately before merge

### #17 ParamStore: ack/error mapping
* Milestone: v0.5
* References: [03] §6, [04] §2
* Implementation targets: `include/sitos/status.hpp`, `src/param_store.cpp`,
  `tests/integration/param_store_ack_test.cpp`
* Scope: PutOptions::ack, ack timeout/retry, detailed Status mapping
* Acceptance criteria: integration tests — ack success/failure/timeout, Disconnected/Timeout when StorageNode is stopped
* Depends on: #14, #15

### #106 Shared same-publisher fence primitive
* Milestone: v0.5
* References: [02] §8, [03] §6, [10] §6
* Implementation targets: Transport/StorageNode internal control routing and deterministic
  fake-Transport plus Zenoh integration tests
* Scope: define one reusable in-band token and publisher-identity ordering primitive shared by
  ParamCache local-delivery waits and buffer-application fences
* Acceptance criteria: prior/later write ordering, publisher isolation, duplicate/late marker
  handling, timeout, error preservation, and callback-quiescent lifecycle tests
* Depends on: #14; requires an Accepted ADR before merge

### #139 Mixed Session buffer key contract
* Milestone: v0.4
* References: ADR-0032, [01] C06/X03, [02] §2, [03] §1, [04] §1
* Implementation targets: `include/sitos/key.hpp`, `src/key.cpp`, `tests/unit/key_test.cpp`,
  and the listed documentation reconciliation files
* Scope: append `KeyKind::Buffer` and `ParsedKey::buffer_class`, add `BufferClass` and
  `BuildBufferKey(prefix, sid, buffer_class, user_key)`, and parse the canonical durable and
  ephemeral routes. Preserve existing `KeyKind` integral values and five-field positional
  `ParsedKey` aggregate initialization by appending the defaulted member. Five-element structured
  bindings and exhaustive switches may require source changes or rebuild; no binary ABI promise is
  made. Reject undefined enum values and leave non-buffer `buffer_class` disengaged. This stage has
  no StorageNode routing,
  Session lifecycle, capability, factory, interop, RocksDB, package, or CI behavior.
* Acceptance criteria:
  * `BufferKeyTest.BufferRoutesRoundTrip` verifies canonical routes, parsed fields, custom prefix,
    and nullopt for non-buffer keys.
  * `BufferKeyTest.InvalidBufferRoutesAreRejected` verifies malformed routes and undefined enum
    rejection.
  * `BufferKeyTest.BufferClassIsReservedOnlyInTheClassPosition` verifies hierarchical user keys.
* Depends on: #133; implementation precedes #140 and #141.
* Contract Registry: none; the buffer row remains `Normative / Planned / ADR-0032 / —`.

### #140 Unified SessionRecord and admission lifecycle
* Milestone: v0.4
* References: ADR-0032, [02] §4, [04] §3, [06] §5.1.1
* Implementation targets: `include/sitos/storage_node.hpp`, `include/sitos/session.hpp`,
  `src/storage_node.cpp`, `src/session_view.cpp`, focused lifecycle tests, and existing sanitizer
  test selection
* Scope: replace separate Session ownership tables with generation-safe Creating/Active/Closing
  SessionRecord admission while preserving existing parameter, snapshot, overlay, and SessionView
  behavior. Enforce callback-gate and lock-order contracts, rollback, quiescent Close, Stop
  ordering, and stale same-SID View rejection. Do not add buffer capability or routing behavior.
* Acceptance criteria:
  * `StorageNodeSessionLifecycleTest.CreatingAndClosingCollisionsAreDeterministic`
  * `StorageNodeSessionLifecycleTest.CreateRollbackAllowsSameSidRetry`
  * `StorageNodeSessionLifecycleTest.CloseQuiescesAdmittedOperations`
  * `StorageNodeSessionLifecycleTest.DestroysResourcesBeforeCloseReturns`
  * `StorageNodeSessionLifecycleTest.StopWaitsForAdmittedCreate`
  * `SessionViewTest.CloseWaitsForCapturedRead`
  * `SessionViewTest.StaleViewRejectsSameSidReplacement`
* Depends on: #139; implementation precedes #141 and final #56.
* Contract Registry: none.

### #141 Mixed Session buffer capability and routing
* Milestone: v0.4
* References: ADR-0032, [01] C06/X01/X03, [02] §2/§4, [04] §3, [06] §5.1.1
* Implementation targets: `include/sitos/session.hpp`, `include/sitos/storage_node.hpp`,
  `src/storage_node.cpp`, focused buffer API/lifecycle/routing tests, and sanitizer test selection
* Scope: add SessionOptions and the node-level C++ DurableBufferEngineFactory, then implement
  deterministic in-process capability admission and durable/ephemeral routing on #140. Apply the
  owner-approved factory taxonomy, bare `zenoh/bytes` validation, engine-authoritative write-once
  handling, query selectors, exception containment, and no node-retained ephemeral state. Do not
  add raw-Zenoh, RocksDB, package, combined-CI, or production subscriber APIs.
* Acceptance criteria:
  * The fixed API, lifecycle, and routing tests from [06] §5.1.1 pass for all four capability
    combinations and all DEC-56 failure, encoding, persistence, and Stop contracts.
  * Disabled durable capability creates or mutates no engine state; disabled ephemeral capability
    retains no node state; a false/throwing engine collection before reply dispatch produces zero replies,
    while a reply-handler failure after dispatch begins suppresses later replies and may preserve earlier replies.
* Depends on: #139 and #140; implementation precedes final #56.
* Contract Registry: none; the buffer row remains Planned.

### #56 Session-scoped buffer integration
* Milestone: v0.4
* References: ADR-0032, [01] F03/F04/F10/N05/N07/C03/C06/X01/X03, [02] §2/§4,
  [03] §1/§4, [06] §5.1.1
* Implementation targets: raw-Zenoh buffer fixture and Python controller, focused RocksDB buffer
  lifecycle tests, installed consumer validation, `tests/vcpkg/validate_vcpkg_provisioning.cmake`,
  `.github/workflows/ci.yml`, and the final documentation/Contract Registry transition
* Scope: integrate #139, #140, and #141 with process-isolated raw-Zenoh interoperability, test-only
  durable late join, RocksDB lifecycle evidence, combined Zenoh-ON/RocksDB-ON Windows and Linux
  validation, and installed configure/build/link consumers. Preserve existing Zenoh-ON/RocksDB-OFF
  and RocksDB-ON/Zenoh-OFF jobs. The buffer Registry row remains Contract `Normative`,
  Implementation `Planned` until this final #56 PR, then `Implemented`, Normative spec `ADR-0032`,
  and Design authority `—`. Do not add BufferPublisher/BufferSubscriber or absorb Issues #105-#108.
* Acceptance criteria:
  * Revalidate the merged #140 lifecycle and #141 capability/routing contracts at the integration
    boundary without duplicating their deterministic in-process factory, lifecycle, capability, or
    routing acceptance suites. Their fixed tests remain owned by #140 and #141.
  * Reuse #29's process-isolated fixture safeguards: one Transport/session topology as applicable,
    bounded readiness and command handshakes, independent stdout/stderr draining, failure-safe
    cleanup, unique identifiers, port-race recovery, and hash-locked Python dependencies.
  * `BufferLateJoinTest.OrdersMaterializedBufferedAndLiveSamples` and
    `BufferLateJoinTest.DurableLateJoinDoesNotLoseDistinctKeys` prove materialized Get replies,
    then buffered samples, then post-transition live samples, with only documented same-byte
    duplicate handling. `FailureInvokesNoObserverAndCleansUp` proves failure cleanup.
  * `RawZenohClientCanUseMixedSessionBuffers`,
    `RawZenohDurableLateJoinPreservesDistinctKeys`, and
    `RawZenohBufferInteropFixtureBoundaries` prove raw-client interoperability using canonical
    routes, bare `zenoh/bytes`, capability behavior, and the durable late-join boundary.
  * `RocksDBBufferLifecycleTest.CloseReleasesEngineBeforeReturn` and
    `RocksDBBufferLifecycleTest.SameSidRecreationUsesFreshEngine` prove RocksDB-specific release
    ordering and fresh recreation without claiming restart retention or #108 catalog behavior.
  * Installed Linux and Windows consumers configure, build, and link against the combined package;
    they do not execute `RocksDBEngine::Open`. Wheels remain RocksDB-free and no fixture binary
    enters installed production artifacts.
  * Combined Windows/Linux Zenoh-ON+RocksDB-ON validation passes while preserving
    Zenoh-ON/RocksDB-OFF and RocksDB-ON/Zenoh-OFF/vcpkg configurations. Buffer payloads remain
    plain `zenoh/bytes`.
  * Validation classification is explicit and follows published `DEC-56-TDD-EXCEPTION-001`: the
    six attempted late-join/raw RED runs are retained as partial/incorrect factual evidence and do
    not satisfy the TDD gate; every corrected or newly strengthened assertion for those six cases
    is review-driven regression. The two RocksDB lifecycle cases and inherited #139-#141 checks are
    review-driven regression; combined package/platform execution is co-developed integration
    coverage; workflow/docs-only checks are N/A with a recorded reason. No RED history may be
    fabricated or relabeled.
* Depends on: #8, #12, #29, #121, #139, #140, #141
* F10 applies here as the Session resource-release principle: CloseSession releases the durable
  buffer engine and other owned resources after callback quiescence.
* Boundaries: #105 owns durability barriers; #106 owns publisher fences; #107 owns applied and
  synchronized BufferPublisher fences; #108 owns restart catalogs, retention, orphan handling,
  and deletion retry. #56 does not add those mechanisms or Python buffer APIs.

### #107 BufferPublisher applied and synchronized fences
* Milestone: v0.5
* References: ADR-0032, [02] §8, [04]
* Implementation targets: C++ and Python BufferPublisher APIs plus deterministic/integration tests
  over `buffers/<sid>/{durable|ephemeral}/**`
* Scope: explicit application-controlled `Push` plus applied or synchronized `Fence`; no automatic
  per-value fence or application-specific manifest policy; Python parity is mandatory v0.5 scope
  because Python Holoscan/CuPy Workers require the same terminal manifest-and-fence sequence
* Acceptance criteria: applied and synchronized receipts, failure and timeout propagation,
  publisher isolation, restart behavior, C++/Python API parity, contiguous NumPy input, and payload
  lifetime safety
* Depends on: #27, #56, #105, #106

### #108 Restart-safe retained-session catalog
* Milestone: v0.5
* References: ADR-0032, [02] §7, [10] §6
* Implementation targets: StorageNode durable-session catalog, startup recovery, and
  crash/restart integration tests
* Scope: retain and recover durable buffer sessions without making ephemeral sessions restartable.
  Retention is not a storage-wide write barrier; host applications enforce external write policy.
  Catalog corruption fails closed: sitos returns a typed catalog-unavailable error and refuses
  catalog-dependent reads and mutations; v0.5 provides no partial service and performs no
  automatic or in-process repair or recovery. The host latches readiness false; recovery requires
  offline maintenance and restart
* Acceptance criteria: durable recovery, ephemeral exclusion, explicit close non-resurrection,
  degraded fail-closed handling of missing or corrupt storage, and concurrent startup/close safety
* Depends on: #8, #12, #56, #105; requires an Accepted ADR before merge

---

## M3: ParamCache and SessionView

### #18 ParamCache: initial fetch + delta subscription
* Milestone: v0.2
* References: [02] §5, [04] §4
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`,
  `tests/integration/param_cache_attach_test.cpp`
* Scope: Attach(sid)/Detach, loss-prevention sequence (a)→(b)→(c), shared_ptr-based cache,
  delta application; the former AttachBase mode is superseded by #100
* Acceptance criteria: integration tests — final state is correct even with a concurrent put during Attach
  (race reproduction test), batch apply, subscription stops after Detach
* Depends on: #5, #12, #13

### #100 ParamCache: remove AttachBase mode
* Milestone: v0.2
* References: [02] §5, [04] §4, ADR-0022
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`,
  `tests/unit/param_cache_attach_test.cpp`, `tests/integration/param_cache_attach_test.cpp`
* Scope: remove the public AttachBase API and all ParamCache base-mode paths; require an
  explicit syntactically valid session id without session-existence preflight; preserve #18
  lifecycle and callback guarantees; ADR-0022 is Accepted after merge
* Acceptance criteria: build-tree and installed consumers prove AttachBase is absent; session
  lifecycle/concurrency tests remain green; no ParamCache base selector is declared or queried;
  Zenoh-OFF/ON package validation passes
* Depends on: #18; blocks #19

### #19 ParamCache: zero-copy reads and performance
* Milestone: v0.2
* References: [04] §4, [01] N01
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`,
  `tests/unit/param_cache_read_test.cpp`, `tests/bench/cache_get_bench.cpp`
* Scope: Result-bearing GetShared/Get/GetOr/GetSpan (SpanHandle), Contains, raw-prefix lexical
  List, session-only Put/PutBatch, atomic active-State publication, operation lease, shared public
  `param_concepts.hpp` and `list_sink.hpp`, deterministic lifecycle/concurrency tests, and opt-in
  benchmark harness
* Acceptance criteria: local reads perform no Transport calls or deep payload copies; List invokes
  caller sinks after releasing cache locks; SpanHandle survives overwrite/Detach/destruction;
  writes submit first and apply locally on success; same-session Zenoh propagation and other-sid
  isolation pass; WSL2 Release Zenoh-OFF scalar Get/GetSpan medians are below 1,000 ns/op and no
  more than four times direct-lookup baselines
* Depends on: #100 (completed)

### #20 Disconnect/reconnect recovery
* Milestone: v1.0
* References: [02] §9, [01] N10
* Implementation targets: `src/param_cache.cpp`,
  `tests/integration/reconnect_test.cpp`
* Scope: stale flag, approved liveness detection, and automatic refetch; the Issue remains blocked
  until its failure model and Transport signal are defined
* Acceptance criteria: integration — ParamCache recovers across the approved restart/failure model
* Depends on: #19

### #99 ParamCache local-delivery fence
* Milestone: v0.5
* References: [02] §5, [04] §4
* Implementation targets: `include/sitos/param_cache.hpp`, `src/param_cache.cpp`, and deterministic
  fake-Transport plus Zenoh integration tests
* Scope: expose `WaitForLocalDelivery(timeout)` using the shared same-publisher fence primitive;
  wait only for the initiating cache subscriber, not peers or StorageNode acknowledgements
* Acceptance criteria: prior local writes observed, later writes excluded, timeout/error mapping,
  concurrent waiter isolation, and Detach/move/destruction quiescence
* Depends on: #19, #106

### #21 SessionView (host-process facade)
* Milestone: v0.2
* References: [04] §5
* Implementation targets: `include/sitos/session_view.hpp`, `src/session_view.cpp`,
  `tests/unit/session_view_test.cpp`
* Scope: move-only read-only in-process resolving reads from overlay → snapshot; Result-bearing
  Get/GetOr/Contains/List, lifecycle and composite-read consistency, ADR-0025
* Acceptance criteria: unit + integration — resolution order, malformed-payload behavior, lexical
  materialized List, lifecycle/ownership/Stop safety, and synchronized consistency with ParamCache
* Depends on: #12

---

## M4: Python bindings

### #22 nanobind module skeleton and wheel build
* Milestone: v0.3
* References: [05], [06] §3
* Implementation targets: `python/pyproject.toml`, `python/bindings/module.cpp`,
  `python/sitos/__init__.py`, `tests/python/test_import.py`, `.github/workflows/wheels.yml`
* Scope: `_sitos` module, scikit-build-core, non-publishing cibuildwheel configuration,
  explicit `SITOS_ZENOHC_ROOT` staging, and ParamValue ↔ Python type conversion
* Acceptance criteria: repaired CPython 3.12 manylinux_2_28 x86_64 production-wheel installation on
  Ubuntu 24.04 and Rocky Linux 10, plus non-publishing CPython 3.12 Windows build/test coverage;
  exact-filename `--only-binary=:all:` installation without Rust or a C++ toolchain; pytest for type
  conversion (shared golden fixtures); no RocksDB or build artifacts in the wheel
* Depends on: #4 (can proceed in parallel; preferably after #15/#18 APIs settle)

### #23 Python ParamStore
* Milestone: v0.3
* References: [05] §2.1
* Implementation targets: `python/bindings/param_store.cpp`, `python/sitos/store.py`,
  `tests/python/test_param_store.py`
* Scope: non-callback Python binding for ParamStore, context manager, and exception mapping;
  Subscribe belongs to #26 and acknowledgement options belong to #17
* Acceptance criteria: pytest — Put/Get/List/Delete/Contains round-trip and deterministic close
* Depends on: #15, #22

### #24 Python ParamCache
* Milestone: v0.3
* References: [05] §2.2, ADR-0022, ADR-0023
* Implementation targets: `python/bindings/param_cache.cpp`, `python/sitos/cache.py`,
  `tests/python/test_param_cache.py`, `tests/python/test_param_cache_live.py`
* Scope: session-only Python binding for ParamCache reads/writes, attach/detach, terminal quiescent
  close, and the shared Issue #23 exception/conversion boundary; no AttachBase, stale/reconnect,
  callback, NumPy, buffer-protocol, or delivery-barrier surface
* Acceptance criteria: fixture-free repaired-wheel pytest plus source-only deterministic
  attach/concurrent-delta, peer-propagation, GIL-progress, and close-quiescence scenarios using one
  non-installed C++ fixture process
* Depends on: #19, #22, #23

### #25 Python StorageNode / SessionView
* Milestone: v0.3
* References: [05] §2.3–2.4, ADR-0025
* Implementation targets: `python/bindings/storage_node.cpp`, `python/bindings/session_view.cpp`,
  `python/sitos/node.py`, `tests/python/test_storage_node.py`
* Scope: Python bindings for opaque InMemoryEngine, immediately started StorageNode, and read-only
  SessionView; supported integration uses one independently opened Zenoh session per spawned process
  for the coordinator ParamCache, StorageNode/SessionView worker, and ParamStore writer
* Acceptance criteria: pytest — lifecycle/quiescence, exact typed reads and eager `items`,
  overlay-over-snapshot resolution, deterministic source-only GIL heartbeat barriers, and bounded
  independently observed cross-component synchronization
* Depends on: #21, #22, #23, #24, #27

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
* Implementation targets: `python/bindings/numpy_binding.cpp`, `python/sitos/py.typed`,
  `python/sitos/*.pyi`, `tests/python/test_numpy_zero_copy.py`
* Scope: exact-ndarray input conversion, `ParamCache.get_array(key, *, dtype=...)`
  (writeable=False, keepalive), and public `.pyi` stubs; no general buffer protocol
* Acceptance criteria: fixture-free repaired-wheel NumPy tests, source-only zero-copy and
  keepalive tests, and strict installed-wheel mypy consumer validation
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
* Scope: Verify `:batch` and ack using only zenoh-python + struct
* Acceptance criteria: batch/ack test written only from the wire specification is green
* Depends on: #13, #14

### #31 C++ examples and demo binary sitobolon
* Milestone: v0.4
* References: [00] §4, [06] §1
* Implementation targets: `CMakeLists.txt`, `examples/cpp/CMakeLists.txt`,
  `examples/cpp/quickstart.cpp`, `examples/cpp/sitobolon.cpp`,
  `tests/examples/test_cpp_examples.py`, and existing CI/vcpkg/docs seams
* Scope: default-OFF C++ quickstart tutorial and standalone StorageNode executable
  `sitobolon`, with complete JSON5 pass-through and InMemory/RocksDB engine selection.
  Examples require Zenoh and are never installed.
* Acceptance criteria: standard Linux/Windows jobs run the four non-RocksDB CTests;
  combined Zenoh+RocksDB vcpkg lanes run all five fixed CTests and prove example binaries are absent
  from the install. The process driver uses bounded, no-sleep readiness and signal cleanup.
* Evidence: executable/process cases and combined-platform execution are co-developed integration
  coverage; the configure guard is compile/contract RED only when asserted before implementation;
  CI wiring and documentation reconciliation are N/A for behavioral RED.
* Depends on: #8, #15, #18, #19, #121

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
* Implementation targets: `NOTICE`, `.github/release-please-config.json`, `pyproject.toml`
* Scope: NOTICE, complete the pre-publication checklist, reserve PyPI/crates names, release-please,
  and add publication to the non-publishing wheel validation owned by Issue #22
* Acceptance criteria: All checklist items completed, dry-run publish to TestPyPI succeeds
* Depends on: M4 completed, #34

### #109 Align the roadmap and retire the optional HTTP gateway
* Milestone: v0.5
* References: ADR-0015, ADR-0027, [06], [10]
* Implementation targets: ADR index and release/build/issue-breakdown documentation
* Scope: record host ownership of HTTP control planes, supersede ADR-0015 without rewriting its
  history, remove the planned gateway build surface, and align the v0.5 roadmap with GitHub
* Acceptance criteria: ADR-0027 is Accepted, live documentation has no planned sitos gateway, and
  the release boundaries and dependencies match the GitHub issues and milestones
* Depends on: none

---

## Recommended implementation order (parallel lanes)

```text
Lane A (core):       #1 → #4 → #6 → #7 → #8 → #105
Lane B (zenoh):      #2 → #3 → #9 → #10 → #11 → #12 → #18 → #19/#20
                                             ├→ #13 → #15/#16
                                             ├→ #14 → #17
                                             │       └→ #106 → #99
                                             └→ #56 → #107/#108
Lane C (Python):     #22 → #23 → #24 → #27 → #25
                     Advanced callbacks/engines: #26/#28 (v1.0)
Lane D (quality):    #29/#30, #31/#32, #33, #34/#35 (as dependencies complete)
Lane E (roadmap):    #109 first; host applications own HTTP control planes under ADR-0027
```

Each Issue is assumed to correspond to one PR. The PR must describe the corresponding requirement IDs
and the AC verification results.

Legacy API compatibility layers and migration work for specific products are proprietary-side work,
so they are not included in this Issue list.

(END OF DOCUMENT)
