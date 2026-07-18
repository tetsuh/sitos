# sitos — Build / Test / Packaging

## 1. Repository layout

```
sitos/
  CMakeLists.txt              # Top level. Options: SITOS_WITH_ROCKSDB,
                              #   SITOS_BUILD_PYTHON, SITOS_BUILD_TESTS, SITOS_BUILD_EXAMPLES,
                              #   SITOS_ENABLE_TSAN, SITOS_ENABLE_ASAN_UBSAN,
                              #   SITOS_BUILD_GATEWAY (optional HTTP gateway, ADR-0015)
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
  - **RocksDB** (when `SITOS_WITH_ROCKSDB=ON`): `find_package(RocksDB)`.
    Supplied by vcpkg / apt / brew
  - **cpp-httplib** (when `SITOS_BUILD_GATEWAY=ON`): header-only, MIT. Pulled only
    for the optional `sitos-gateway` component; the core build is unaffected when OFF [ADR-0015]
  - **gtest / benchmark**: FetchContent
* Presets: define `dev-windows`, `dev-linux`, `release`, and `python-wheel` in
  `CMakePresets.json`

## 3. Install (C++ consumer)

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

* scikit-build-core + nanobind. Generate wheels with `python -m build`
* CI build with cibuildwheel: `cp310–cp313 × {win_amd64, manylinux_2_28_x86_64}` [P03]
* Standard wheels bundle zenoh-c and do not include RocksDB
  (Linux: auditwheel repair, Windows: delvewheel).
  RocksDBEngine is separated as a `sitos-rocksdb` wheel or a future optional extra.
* Runtime dependency: `numpy>=1.24`

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
| `SnapshotFallbackCopiesForInMemory` | N03 | InMemory snapshot works with the same semantics |
| `AttachDoesNotMissConcurrentPut` | F06 | Does not miss a concurrent put during Attach |
| `BatchIsReceivedBySessionSubscriber` | F09 / ADR-0018 | `:batch` is received by a `session/<sid>/**` subscription |
| `SpanHandleSurvivesOverwrite` | N01/P02 | old SpanHandle/ndarray remains valid after an update |
| `RawZenohClientCanPutAndGet` | C03 | Single-value interoperability using only zenoh-python |
| `RawZenohClientCanSendBatch` | C03/F09 | Batch interoperability using only zenoh-python |
| `PutAckTimesOutWhenNodeUnavailable` | N10 | ack timeout/status mapping |
| `PythonCallbackDoesNotDeadlockWithGet` | P04 | get inside callback does not deadlock |

## 5.2 Lifecycle sanitizer runs

Issue #11 lifecycle tests, Issue #13 batch sequencing tests, and Issue #18 ParamCache
fake-Transport lifecycle tests have reproducible sanitizer configurations. TSan runs the
zenoh-independent fake-Transport paths; ASan/UBSan runs the same paths separately:

```sh
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSITOS_BUILD_TESTS=ON -DSITOS_WITH_ZENOH=OFF -DSITOS_ENABLE_TSAN=ON
cmake --build build/tsan
ctest --test-dir build/tsan --output-on-failure \
  -R 'StorageNodeLifecycleTest|StorageNodeSessionTest|StorageNodeBatchTest|ParamCacheTest'

cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSITOS_BUILD_TESTS=ON -DSITOS_WITH_ZENOH=OFF -DSITOS_ENABLE_ASAN_UBSAN=ON
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure \
  -R 'StorageNodeLifecycleTest|StorageNodeSessionTest|StorageNodeBatchTest|ParamCacheTest'
```

For a platform where the zenoh-c standalone runtime supports sanitizer instrumentation,
repeat the ASan/UBSan configuration with `-DSITOS_WITH_ZENOH=ON` and run the lifecycle
integration targets. The CI sanitizer job uses zenoh OFF to keep TSan independent of
zenoh runtime internals.

## 6. CI (GitHub Actions)

| workflow | Trigger | Contents |
|---|---|---|
| `ci.yml` | PR/push | Windows (MSVC) + Linux (gcc, clang) builds; unit + integration + python + interop; clang-format/clang-tidy; mypy |
| `wheels.yml` | tag / nightly | cibuildwheel → PyPI (on tag) / TestPyPI (nightly) |
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
