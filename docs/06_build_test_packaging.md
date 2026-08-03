# sitos — Build / Test / Packaging

## 1. Repository layout

```
sitos/
  CMakeLists.txt              # Top level. Options: SITOS_WITH_ROCKSDB,
                              #   SITOS_BUILD_PYTHON, SITOS_BUILD_TESTS, SITOS_BUILD_EXAMPLES,
                              #   SITOS_BUILD_BENCHMARKS, SITOS_ENABLE_TSAN, SITOS_ENABLE_ASAN_UBSAN
  cmake/                      # zenoh-c integration (FetchContent/Corrosion/find_package)
  include/sitos/              # Public headers (API from 04_api_cpp.md)
  src/                        # Implementation
  python/
    pyproject.toml            # scikit-build-core
    bindings/                 # nanobind module (_sitos)
    sitos/                    # Pure Python layer + .pyi
  tests/
    unit/                     # gtest (prefer tests based on InMemoryEngine and not requiring zenoh)
    integration/              # Multiprocess / via zenoh
    interop/                  # Interoperability tests that speak only zenoh-python [C03]
    bench/                    # Benchmarks (Google Benchmark)
    python/                   # pytest
  examples/
    cpp/                      # quickstart, storage_node demo (binary name: sitobolon)
    python/
  docs/                       # English design documents + Doxygen/Sphinx
  .github/workflows/
```

## 2. Build (C++)

* C++20, maximum warning level + warning-as-error (MSVC `/W4 /WX`, gcc/clang `-Wall -Wextra -Werror`)
* **Code style** (D12): `.clang-format` is `BasedOnStyle: Google` +
  `ColumnLimit: 100`. Naming follows the Google C++ Style Guide
  (types/methods = PascalCase, variables = snake_case, members = trailing `_`,
  constants = kPascalCase). File names are snake_case (`param_value.hpp`)
* **Language** (D9): code, comments, commit messages, Issues/PRs, and docs are English.
  This design document set is the English edition in `docs/`
* Dependency resolution:
  - **zenoh-c**: First choice is to use official prebuilt releases via
    `find_package(zenohc)`. CI pins versions with FetchContent.
    Also provide a Corrosion (Rust) path for environments that need source builds
  - **RocksDB** (when `SITOS_WITH_ROCKSDB=ON`): `find_package(RocksDB 11.1.2 EXACT CONFIG REQUIRED)`.
  - **gtest / benchmark**: FetchContent
* Presets: define `dev-windows`, `dev-linux`, `release`, and `python-wheel` in
  `CMakePresets.json`

### 2.1 Optional vcpkg provisioning foundation

Optional native dependencies are provisioned explicitly through the root `vcpkg.json` manifest.
The manifest has no ordinary dependencies and keeps RocksDB behind the non-default `rocksdb`
feature. CI checks out the official `microsoft/vcpkg` repository detached at the manifest's
`builtin-baseline`, verifies `HEAD` against that value, and bootstraps with telemetry disabled.
The baseline is the sole executable and registry pin source; updates require a dedicated
dependency-update PR that changes the baseline and expected probe version together.

The canonical validation triplets are the built-in `x64-linux` and `x64-windows` triplets. Configure
opt-in consumers with `VCPKG_MANIFEST_FEATURES=rocksdb` and a fresh explicit
`VCPKG_INSTALLED_DIR`; ordinary builds, installed package validators, and standard wheels do not
use the vcpkg toolchain and retain `SITOS_WITH_ROCKSDB=OFF`. Project CMake and installed package
configuration never invoke vcpkg or another package manager and never perform hidden provisioning.
Existing scoped Zenoh and Google Benchmark `FetchContent` paths are unchanged.

Dedicated vcpkg CI jobs persist a per-platform filesystem binary archive with the GitHub-owned
`actions/cache` action pinned to an exact commit and configure vcpkg with
`clear;files,<directory>,readwrite`. Primary keys separate the cache format, OS, architecture,
canonical triplet, root manifest hash, and GitHub-hosted runner image identity; vcpkg package ABI
remains the inner correctness boundary. A compatible-prefix restore may seed a new runner-image key,
but only an exact primary-key hit skips save. Restore occurs before provisioning, while save occurs
only after provisioning, configure, build, runtime, no-feature, and static-guard validation succeeds.
Pull-request writes remain isolated to their merge refs, trusted `main` pushes use their normal cache
scope, and no secret or external credential is used. Cache hits affect performance only;
provisioning, configure, build, and runtime failures remain fatal. The initial cold-provisioning
budgets are 180 minutes on Linux and 360 minutes on Windows.

The first successful canonical GitHub-hosted PR run was run `30236511593`,
attempt 1: Linux job
[89885220722](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89885220722) emitted
`VCPKG_PROVISIONING_WALL_SECONDS=970.42`, and Windows job
[89885220723](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89885220723) emitted
`VCPKG_PROVISIONING_WALL_SECONDS=2,491.543` (2,491.543 seconds). These are provisioning-command
wall times, not full job durations: the Linux job ran approximately 1,595 seconds and the Windows
job approximately 2,531 seconds.

The same PR merge-ref was rerun as run `30236511593`, attempt 3. Linux job
[89961622937](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89961622937) emitted
`VCPKG_PROVISIONING_WALL_SECONDS=1010.06`, and Windows job
[89961622965](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89961622965) emitted
`VCPKG_PROVISIONING_WALL_SECONDS=2,802.580` (2,802.580 seconds). The rerun full job durations were
approximately 1,043 and 2,840 seconds respectively. Both runs explicitly report that the pinned
vcpkg executable's `x-gha` binary caching backend "has been removed"; no cache restore, hit, miss, or
save evidence is present. These historical runs explain the replacement but do not validate it.

PR #127 uses the SHA-pinned GitHub cache action with vcpkg's `files` backend. Run `30277962785`,
attempt 1, recorded cache misses, complete successful validation, and archive saves: Linux took
951.12 seconds and Windows took 2,819.379 seconds. Attempt 2 on the same head restored the exact
keys, retained all validation, and skipped save: Linux restored approximately 210 MB and took 24.96
seconds, while Windows restored approximately 594 MB and took 73.277 seconds. The provisioning
steps were about 38.1 and 38.5 times faster respectively. These measurements used the first
filesystem-key schema, which lacked the runner-image boundary.

Run `30315682979`, attempt 1, restored those v1 archives through migration restore keys and saved
runner-image-aware v2 primary keys after all validation passed. Linux provisioning took 33.24 seconds
for image `ubuntu24-20260720.247.2`; Windows took 61.910 seconds for image
`win25-vs2026-20260714.173.1`. Attempt 2 restored the exact v2 keys, reported
`VCPKG_BINARY_CACHE_HIT=true`, retained all validation, skipped save, and completed provisioning in
23.24 seconds on Linux and 60.623 seconds on Windows. Cache entries are immutable and may be evicted,
so later cold builds remain expected.

On Windows, the canonical `RocksDB::rocksdb` target is the `/MD`-compatible static archive. The
probe stages and verifies the baseline's canonical `z.dll` in a clean runtime directory. The
validator requires `dumpbin` from the `vcvars64.bat` environment. It does not rename or duplicate
`z.dll` as `zlib1.dll`, depend on `rocksdb-shared.dll`, or use build-tree/vcpkg PATH entries.

## 3. Install (C++ consumer)

When `SITOS_WITH_ROCKSDB=ON`, the installed package records the exact RocksDB version used to build
sitos and reconstructs `RocksDB::rocksdb` with `find_dependency(RocksDB <version> EXACT CONFIG)`.
RocksDB-ON installed consumers are configured and built/linked but not executed; runtime deployment
belongs to the application or package manager. Exact version equality is necessary but not sufficient
for ABI compatibility, so external consumers must provide a compatible compiler, CRT, standard
library ABI, build configuration, and compile options. See ADR-0033.

The C++ library and public headers can be installed with the generated CMake export:

```sh
cmake --install build/release --prefix /opt/sitos
```

This installs `include/sitos/*`, the static library, `sitosTargets.cmake`,
`sitosConfig.cmake`, and the version file under the platform's
`${CMAKE_INSTALL_LIBDIR}/cmake/sitos` directory (commonly `lib/cmake/sitos`).
Consumers can use the installed package through the exported target. Because sitos is currently
pre-1.0, the generated version file uses `SameMinorVersion`: a `0.1.x` consumer requirement
accepts only `0.1` patch releases, not `0.2`.

```sh
cmake -S consumer -B build/consumer -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/sitos
cmake --build build/consumer
```

The consumer uses `find_package(sitos CONFIG REQUIRED)` and links
`sitos::sitos`. The package version file uses `SameMinorVersion` while sitos remains pre-1.0.
Zenoh-OFF packages have no Zenoh dependency. Zenoh-ON packages require an
externally provisioned zenoh-c standalone tree discoverable through
`zenohc_ROOT`, `ZENOHC_ROOT`, or a normal CMake prefix; downstream package
discovery never downloads zenoh-c. The application or package manager must
deploy `zenohc.dll` or `libzenohc.so` at runtime.

## 4. Build (Python wheel)

Issue #22 owns non-publishing wheel build and validation. Issue #35 owns PyPI/TestPyPI
publication. The wheel build uses the repository root CMake project through the `python/`
`pyproject.toml`:

```sh
python -m build --wheel python --outdir dist
```

scikit-build-core installs only the `python` CMake component into the wheel. Build directories are
specific to the wheel tag (`../build/python-wheel/{wheel_tag}`), so CPython builds cannot reuse one
another's CMake cache. The CMake project version is the single source for wheel metadata and
`sitos.__version__`. Python build isolation installs NumPy from `python/pyproject.toml` so CMake
can locate the NumPy C headers; NumPy remains a runtime dependency. Public `py.typed` and `.pyi`
files are installed into the wheel, and the wheel jobs run a strict mypy consumer outside the
checkout while keeping mypy out of runtime metadata. NumPy 2.x is the supported C API and runtime
line; NumPy 1.x compatibility shims are intentionally not maintained. The publishing wheel jobs
build and test against the declared NumPy 2.0 minimum, while the non-publishing latest-compatible
lane checks dependency drift.

The production target is `cp312-manylinux_2_28_x86_64`, validated on Ubuntu 24.04 and Rocky Linux
10. `cp312-win_amd64` receives non-publishing build/test coverage only; other Python versions and
formal Windows publication are deferred. Standard wheels bundle exactly one zenoh-c runtime and do
not include RocksDB (Linux: auditwheel repair, Windows: delvewheel). Linux wheels build the pinned
zenoh-c source in the manylinux_2_28 builder with the pinned Rust 1.93.0 toolchain; the build script
replaces the archive's stale Cargo.lock with the versioned repository-owned lock artifact and runs
`cargo --locked`. The result is staged in the standalone layout and passed to CMake as
`-DSITOS_ZENOHC_ROOT=/opt/zenohc-stage`. Windows stages the pinned official standalone archive with
the same cache variable. When the variable is empty, normal C++ builds retain the existing pinned
FetchContent path. CMake validates staged headers and native runtime files before configuring.

To test a repaired wheel without a compiler or source-tree import, select its exact filename and use
an isolated environment. CI first hash-installs fetched test/runtime dependencies from
`.github/wheel-tools-requirements.txt`, records the generated wheel SHA-256, and installs that local
artifact with `--no-deps`; a generated artifact cannot have a pre-build hash.

```sh
project_root="$(pwd)"
python -m venv /tmp/sitos-wheel-test
/tmp/sitos-wheel-test/bin/python -m pip install --only-binary=:all: --require-hashes \
  -r .github/wheel-tools-requirements.txt
sha256sum dist/<exact-wheel>.whl
/tmp/sitos-wheel-test/bin/python -m pip install --no-deps --only-binary=:all: dist/<exact-wheel>.whl
(cd /tmp && /tmp/sitos-wheel-test/bin/python -m pytest "$project_root/tests/python")
```

On Windows, use `C:\\path\\to\\sitos-wheel-test\\Scripts\\python.exe` instead. Inspect the wheel
with `python scripts/check_wheel.py --platform linux dist/<exact-wheel>.whl` (or `--platform windows`);
the check derives the CMake version and rejects RocksDB, GoogleTest, GoogleMock, SDK/CMake exports,
and build-tree artifacts and requires `_sitos` plus exactly one zenoh-c runtime. The installed wheel
does not require Rust, CMake, Ninja, or a C++ compiler.

The standard wheel runtime dependency is `numpy>=2.0`.

## 5. Test strategy

| Layer | Framework | Target | Run in CI |
|---|---|---|---|
| unit | gtest | ParamValue encode/decode (payload v1 golden tests), StorageEngine contract tests (common test suite instantiated for each engine), key validation, ParamStore validation/read semantics, Overlay resolution | Always |
| integration | gtest | Connect StorageNode + ParamStore + ParamCache through one injected same-process Transport. Full session lifecycle ([02] §7), ParamStore round trips, disconnect/reconnect [N10], batch, ack | Always |
| multiprocess | gtest + spawn | Attach/delivery/crash recovery with real process isolation | Always (Linux) / nightly (Windows) |
| interop | pytest + zenoh-python | Read/write using only the wire specification ([03]) without the sitos library [C03] | Always |
| python | pytest | API parity, NumPy zero-copy (writeable=False, base-buffer identity), GIL (concurrent get inside callback) | Always |
| bench | Google Benchmark | N01 (cache Get), N02 (TakeSnapshot), N08 (session start), N09 (delivery latency) | nightly + regression comparison in PR comments |

**Contract-test principle**: Write the `StorageEngine` test suite against the abstraction, in a
form reusable for InMemory/RocksDB/(future user engines) [X01].

**Golden tests**: Save the payload v1 byte sequences as fixtures and verify byte-for-byte matches
for encoded results (the cornerstone of compatibility across languages and versions).
The Python side also references the same fixtures.

### 5.1 Required test names

To prevent AI/implementers from misreading the intent, the following test names are fixed for
major behaviors.

| Test name | Target requirement | Verification |
|---|---|---|
| `PayloadV1GoldenFixtures` | C01 | Exact match with the fixtures in [03] §2.3 |
| `BatchV1GoldenFixture` | F09/C01 | Exact match with the batch fixture in [03] §5.1 |
| `InvalidKeysAreRejected` | X03 | Reject reserved characters, empty chunks, and invalid sid |
| `SnapshotIsIsolatedFromBasePut` | F05/N02 | A base put after CreateSession does not affect snap reads |
| `SnapshotIsIsolatedFromBaseDelete` | F05/N02 | A base delete after snapshot creation does not affect snap reads |
| `ListEmitsDeterministicKeyOrder` | X01 | StorageEngine List emits matching keys in ascending order |
| `SnapshotFallbackCopiesForInMemory` | N03 | InMemory snapshot works with the same semantics |
| `AttachDoesNotMissConcurrentPut` | F06 | Does not miss a concurrent put during Attach |
| `BatchIsReceivedBySessionSubscriber` | F09 / ADR-0018 | `:batch` is received by a `session/<sid>/**` subscription |
| `SpanHandleSurvivesOverwrite` | N01/P02 | old SpanHandle/ndarray remains valid after an update |
| `RawZenohClientCanPutAndGet` | C03 | Single-value interoperability using only zenoh-python |
| `RawZenohClientCanSendBatch` | C03/F09 | Batch interoperability using only zenoh-python |
| `PutAckTimesOutWhenNodeUnavailable` | N10 | ack timeout/status mapping |
| `PythonCallbackDoesNotDeadlockWithGet` | P04 | get inside callback does not deadlock |
| `BufferKeyTest.BufferRoutesRoundTrip` | C06/X03 | Build and parse canonical durable and ephemeral routes; custom prefix and non-buffer nullopt |
| `BufferKeyTest.InvalidBufferRoutesAreRejected` | C06 | Reject malformed routes, unknown classes, and undefined BufferClass values |
| `BufferKeyTest.BufferClassIsReservedOnlyInTheClassPosition` | C06 | Permit durable/ephemeral chunks inside a hierarchical user key |

### 5.1.1 Session buffer implementation stages

The Session buffer work is split into four sequential implementation Issues. The fixed tests in
this section are the acceptance-name authority for each stage.

**Stage #139 — mixed buffer key contract**

The three `BufferKeyTest.*` tests above verify only canonical route grammar, parser
classification, invalid-form rejection, class-position reservation, and the custom-prefix portion
of X03. They do not prove bare-byte admission, durable retrieval, ephemeral non-retention, or raw
client interoperability.

**Stage #140 — SessionRecord and admission lifecycle**

- `StorageNodeSessionLifecycleTest.CreatingAndClosingCollisionsAreDeterministic`
- `StorageNodeSessionLifecycleTest.CreateRollbackAllowsSameSidRetry`
- `StorageNodeSessionLifecycleTest.CloseQuiescesAdmittedOperations`
- `StorageNodeSessionLifecycleTest.DestroysResourcesBeforeCloseReturns`
- `StorageNodeSessionLifecycleTest.StopWaitsForAdmittedCreate`
- `SessionViewTest.CloseWaitsForCapturedRead`
- `SessionViewTest.StaleViewRejectsSameSidReplacement`

**Stage #141 — capabilities, factory, and routing**

- `StorageNodeBufferApiTest.PreservesLegacyCreateSessionOverload`
- `StorageNodeBufferApiTest.ExposesCapabilityOverloadAndFactory`
- `StorageNodeBufferLifecycleTest.FactoryFailureTaxonomyAndRollback`
- `StorageNodeBufferLifecycleTest.FactoryAndStopLinearizeDeterministically`
- `StorageNodeBufferLifecycleTest.CloseQuiescesDurableOperationsAndDestroysEngine`
- `StorageNodeBufferLifecycleTest.SameSidRecreationUsesFreshEngine`
- `StorageNodeBufferRoutingTest.CapabilityMatrix`
- `StorageNodeBufferRoutingTest.DurablePutIsByteExactAndWriteOnce`
- `StorageNodeBufferRoutingTest.PutFailureRereadsAuthoritativeEngineState`
- `StorageNodeBufferRoutingTest.WholeSubscriberSerializationPreventsConflictingPuts`
- `StorageNodeBufferRoutingTest.EphemeralPutNeverTouchesEngine`
- `StorageNodeBufferRoutingTest.NonBytesEncodingIsRejected`
- `StorageNodeBufferRoutingTest.DurableQuerySelectorsAndFailures`
- `StorageNodeBufferRoutingTest.EngineFailuresAndExceptionsAreContained`
- `StorageNodeBufferRoutingTest.BufferRoutesDoNotEnterParameterSurfaces`
- `StorageNodeBufferRoutingTest.BufferDeleteAndControlRoutesAreRejected`

These tests verify C06 bare-byte admission, durable Get/List, write-once handling, and absence of
node-retained ephemeral state.

**Final stage #56 — raw interoperability and durable late join**

- `BufferLateJoinTest.OrdersMaterializedBufferedAndLiveSamples`
- `BufferLateJoinTest.DurableLateJoinDoesNotLoseDistinctKeys`
- `BufferLateJoinTest.FailureInvokesNoObserverAndCleansUp`
- `RawZenohClientCanUseMixedSessionBuffers`
- `RawZenohDurableLateJoinPreservesDistinctKeys`
- `RawZenohBufferInteropFixtureBoundaries`
- `RocksDBBufferLifecycleTest.CloseReleasesEngineBeforeReturn`
- `RocksDBBufferLifecycleTest.SameSidRecreationUsesFreshEngine`

These tests verify C03/C06 raw-client interoperability and the process-isolated durable late-join
boundary. The raw tests run against `sitos_raw_zenoh_buffer_fixture`, a process-isolated fixture
that keeps one Transport and uses bounded command/readiness handshakes. The final stage also owns
combined Zenoh-ON/RocksDB-ON package and CI evidence, relocated installed consumers without
executing `RocksDBEngine::Open`, and wheel/installed-artifact guards rejecting both buffer fixture
executable basenames with and without `.exe`.

## 5.2 Lifecycle sanitizer runs

Issue #11 lifecycle tests, Issue #13 batch sequencing tests, Issue #16 ParamStore subscription,
Issue #18 ParamCache, and Issue #21 SessionView lifecycle/concurrency tests have reproducible
sanitizer configurations. TSan runs the
zenoh-independent fake-Transport paths; ASan/UBSan runs the same paths separately:

```sh
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSITOS_BUILD_TESTS=ON -DSITOS_WITH_ZENOH=OFF -DSITOS_ENABLE_TSAN=ON
cmake --build build/tsan
lifecycle_filter="StorageNodeLifecycleTest|StorageNodeSessionTest|"
lifecycle_filter="${lifecycle_filter}StorageNodeSessionLifecycleTest|StorageNodeBufferLifecycleTest|"
lifecycle_filter="${lifecycle_filter}StorageNodeBufferRoutingTest|StorageNodeBufferApiTest|BufferLateJoinTest|StorageNodeBatchTest|"
lifecycle_filter="${lifecycle_filter}TransportGetCompletionTest|ParamStoreSubscribeTest|"
lifecycle_filter="${lifecycle_filter}ParamCacheTest|ParamCacheReadTest|"
lifecycle_filter="${lifecycle_filter}SessionViewTest|SessionViewFixture"
ctest --test-dir build/tsan --output-on-failure --timeout 60 -R "$lifecycle_filter"

cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSITOS_BUILD_TESTS=ON -DSITOS_WITH_ZENOH=OFF -DSITOS_ENABLE_ASAN_UBSAN=ON
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure --timeout 60 -R "$lifecycle_filter"
```

The TSan CI lane additionally repeats the two subscription lifecycle regressions 100 times and the
complete lifecycle selection 10 times. Each sanitizer test has a 60-second CTest timeout, and each
sanitizer job has a 15-minute timeout.

For a platform where the zenoh-c standalone runtime supports sanitizer instrumentation,
repeat the ASan/UBSan configuration with `-DSITOS_WITH_ZENOH=ON` and run the lifecycle
integration targets. The CI sanitizer job uses zenoh OFF to keep TSan independent of
zenoh runtime internals.

## 5.3 C++ examples and demo validation

The C++ examples are opt-in: `SITOS_BUILD_EXAMPLES` remains OFF by default, requires
`SITOS_WITH_ZENOH=ON`, and neither `quickstart` nor `sitobolon` is installed. `quickstart` is an
in-process public-API tutorial. `sitobolon` is a standalone StorageNode process with an InMemory
engine by default and an optional RocksDB engine when that support is built; its complete Zenoh
JSON5 configuration is passed through unchanged.

Example builds register these exact CTests: `CppQuickstartRuns`,
`SitobolonHelpDocumentsOptions`, `SitobolonRejectsInvalidArguments`, and
`SitobolonStartsAndStopsCleanly`; `SitobolonRocksDbReleasesPath` is additionally registered with
RocksDB. Each has a 60-second timeout, and real local Zenoh process tests run serially. Standard
Linux and Windows jobs run the four non-RocksDB tests. The combined Zenoh+RocksDB vcpkg lanes run
all five and assert that installation contains neither example executable. The Python driver uses
only the standard library and bounded process/readiness handling; platform-specific Windows
console-control evidence is obtained in the hosted Windows job.

Evidence classification is explicit: the five executable/process cases are co-developed integration
coverage; the examples-enabled/Zenoh-OFF guard is compile/contract RED only when its assertion is
run before the guard; combined-platform execution is co-developed integration coverage; CI wiring
and documentation reconciliation are N/A for behavioral RED. This records evidence classes without
claiming chronology for work that was co-developed.

## 6. CI (GitHub Actions)

| workflow | Trigger | Contents |
|---|---|---|
| `ci.yml` | PR/push | Windows (MSVC) + Linux (gcc, clang) builds; unit + integration + python + interop; clang-format/clang-tidy; mypy |
| `wheels.yml` | PR / push / manual | cibuildwheel build and repaired-wheel validation; publication is Issue #35 |
| `bench.yml` | nightly / label | Run benchmarks and comment baseline comparisons |
| `dependency-upgrade.yml` | nightly / manual | Build and interop tests with the minimum supported and latest stable zenoh versions. Details: [09_dependency_policy.md](09_dependency_policy.md) |
| `docs.yml` | push main | Doxygen + Sphinx → GitHub Pages |

## 7. Quality gates

* Code coverage: line 80% or higher (gcov/llvm-cov, Codecov)
* sanitizer: ASan/UBSan (Linux CI), TSan nightly for integration
* Static analysis: clang-tidy (modernize-*, bugprone-*, concurrency-*)
* Commit convention: Conventional Commits (automatic CHANGELOG generation with release-please)

## 8. Release

* Semantic versioning [C04]. C++ and Python use the same version
* Artifacts: GitHub Release (source + prebuilt static/shared libraries), PyPI wheel.
  Future: vcpkg / conan registry registration
* Pre-publication checklist: intellectual-property and export-control review, check for inclusion
  of company-specific information, LICENSE (Apache-2.0) / NOTICE / third-party license list

(END OF DOCUMENT)
