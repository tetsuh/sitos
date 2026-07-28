# ADR-0031: Establish a cross-platform vcpkg foundation

## Status

Accepted — 2026-07-28

## Context

sitos needs a reproducible provider for optional native dependencies on Linux and Windows without
changing the default RocksDB-OFF build or adding package-manager behavior to project CMake. The
reviewed vcpkg commit resolves RocksDB 11.1.2, port-version 0, with zlib as its only default
feature. CI must remain correct when its optional binary cache is unavailable.

## Decision

We will use an explicit root `vcpkg.json` with no ordinary dependencies and a non-default `rocksdb`
feature, and use its `builtin-baseline` as the sole executable and registry pin. Dedicated CI jobs
will check out official vcpkg at that baseline and use built-in `x64-linux` and `x64-windows`
triplets. A SHA-pinned GitHub-owned `actions/cache` action persists a dedicated filesystem binary
archive supplied to vcpkg through `clear;files,<directory>,readwrite`. Primary keys include cache
format, OS, architecture, triplet, manifest hash, and GitHub-hosted runner image identity. A
compatible-prefix restore may seed a new runner-image key, but only an exact primary-key hit skips
save. The cache is an optional performance optimization, not a correctness dependency.

## Consequences

* Good: Optional native provisioning is auditable and reproducible across the supported CI platforms.
* Good: Explicit isolated install roots and a consumer probe catch target, link, and runtime errors.
* Good: Pull-request cache writes are constrained to the pull request merge-ref scope, and no
  external cache credential, Mono runtime, NuGet client, or GitHub Packages feed is required.
* Good: vcpkg remains responsible for package-ABI validation inside the cached archive directory.
* Bad: GitHub cache entries are immutable and subject to repository quotas and eviction; the
  runner-image key component or cache-format version changes when the outer compatibility boundary
  changes.
* Bad: Cold provisioning compiles RocksDB and can take substantial time; the initial budgets are 180
  minutes for Linux and 360 minutes for Windows.
* Bad: Homebrew/macOS provisioning is unsupported, and `apt` is limited to Linux CI prerequisites.
* Neutral: Cache misses, eviction, or unavailable credentials do not alter correctness.
* Windows runtime: the canonical static `RocksDB::rocksdb` target imports the pinned zlib 1.3.2
  runtime as `z.dll`; validation stages that actual file, requires `dumpbin` from the `vcvars64.bat`
  environment, and rejects `zlib1.dll` compatibility copies and `rocksdb-shared.dll` dependencies.

## Options Considered

* **Runner-provided vcpkg** — rejected because a mutable checkout is not reproducible.
* **A custom triplet or static Windows triplet** — rejected because this foundation validates the
  canonical built-in triplets and their actual runtime behavior.
* **Automatic provisioning from project or installed CMake** — rejected because package discovery
  must remain network-free and under the caller's explicit provisioning boundary.
* **Removed vcpkg `x-gha` backend** — rejected because the pinned vcpkg executable reports that the
  backend has been removed.
* **GitHub Packages through vcpkg's NuGet backend** — rejected because it adds Mono and `nuget.exe`
  on Linux, feed authentication, and package permissions that are unnecessary for this repository.
* **Caching the installed dependency tree** — rejected because caching vcpkg's package-ABI archives
  retains vcpkg's own reuse and validation boundary.
* **External or long-lived cache credentials** — rejected because GitHub's merge-ref-scoped cache
  action needs no repository secret or external credential.

## References

* Issue #122
* ADR-0021: Resolve installed Zenoh dependencies without fetching or bundling
* `docs/06_build_test_packaging.md`
* `docs/09_dependency_policy.md`

### Validation evidence

#### Superseded `x-gha` attempt

The first successful canonical GitHub-hosted PR run was run `30236511593`, attempt 1:

* Linux job [89885220722](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89885220722)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=970.42`.
* Windows job [89885220723](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89885220723)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=2,491.543` (2,491.543 seconds).

These are provisioning-command wall times, not full job durations. The corresponding full job
spans were 04:14:00–04:30:35 UTC (Linux, approximately 1,595 seconds) and
04:14:01–04:56:12 UTC (Windows, approximately 2,531 seconds).

The same PR merge-ref was rerun as run `30236511593`, attempt 3, with both focused jobs:

* Linux job [89961622937](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89961622937)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=1010.06`.
* Windows job [89961622965](https://github.com/tetsuh/sitos/actions/runs/30236511593/job/89961622965)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=2,802.580` (2,802.580 seconds).

The rerun full job spans were 11:16:38–11:34:01 UTC (Linux, approximately 1,043 seconds) and
11:16:39–12:03:59 UTC (Windows, approximately 2,840 seconds). Both attempts explicitly logged
that the pinned vcpkg executable's `x-gha` binary caching backend "has been removed"; neither log
contains cache restore, hit, miss, or save evidence. Therefore the rerun is a repeat provisioning
measurement, not a warm-cache measurement, and same-PR `x-gha` reuse is not proven. This evidence
is retained as the reason the backend was replaced, not as evidence for the replacement cache.

#### Filesystem binary-cache evidence

PR #127 uses SHA-pinned `actions/cache` restore/save steps around a dedicated vcpkg `files` binary
archive. Run `30277962785`, attempt 1, recorded misses followed by successful post-validation saves:

* Linux job [90016731979](https://github.com/tetsuh/sitos/actions/runs/30277962785/job/90016731979)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=951.12` and saved the Linux archive.
* Windows job
  [90016731735](https://github.com/tetsuh/sitos/actions/runs/30277962785/job/90016731735)
  emitted `VCPKG_PROVISIONING_WALL_SECONDS=2,819.379` and saved the Windows archive.

The same run and head were rerun as attempt 2. Both jobs restored their exact keys, retained all
provisioning and runtime validation, and skipped save because the primary key matched:

* Linux job [90029988641](https://github.com/tetsuh/sitos/actions/runs/30277962785/job/90029988641)
  restored approximately 210 MB and emitted `VCPKG_PROVISIONING_WALL_SECONDS=24.96`, about 38.1
  times faster than the replacement-backend cold run.
* Windows job
  [90029988603](https://github.com/tetsuh/sitos/actions/runs/30277962785/job/90029988603)
  restored approximately 594 MB and emitted `VCPKG_PROVISIONING_WALL_SECONDS=73.277`, about 38.5
  times faster than the replacement-backend cold run.

These measurements used the first filesystem-key schema, which lacked the runner-image boundary.
Run `30315682979`, attempt 1, migrated to the runner-image-aware v2 key without discarding compatible
archives:

* Linux job [90140581635](https://github.com/tetsuh/sitos/actions/runs/30315682979/job/90140581635)
  restored the v1 archive through the migration restore key, reported `cache-hit=false`, completed
  provisioning in 33.24 seconds, and saved
  `vcpkg-files-v2-Linux-X64-x64-linux-...-ubuntu24-20260720.247.2`.
* Windows job
  [90140581599](https://github.com/tetsuh/sitos/actions/runs/30315682979/job/90140581599)
  restored the v1 archive through the migration restore key, reported `cache-hit=false`, completed
  provisioning in 61.910 seconds, and saved
  `vcpkg-files-v2-Windows-X64-x64-windows-...-win25-vs2026-20260714.173.1`.

Attempt 2 reran the exact same head and runner images:

* Linux job [90141182464](https://github.com/tetsuh/sitos/actions/runs/30315682979/job/90141182464)
  restored the exact v2 key, reported `cache-hit=true`, retained all validation, completed
  provisioning in 23.24 seconds, and skipped save.
* Windows job
  [90141182570](https://github.com/tetsuh/sitos/actions/runs/30315682979/job/90141182570)
  restored the exact v2 key, reported `cache-hit=true`, retained all validation, completed
  provisioning in 60.623 seconds, and skipped save.

Cache behavior remains performance evidence only. ADR-0031 was accepted on 2026-07-28.
