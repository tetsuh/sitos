# ADR-0024: Keep Google Benchmark opt-in and outside installed packages

## Status

Proposed — 2026-07-19

## Context

Issue #19 needs reproducible N01 measurements for successful ParamCache scalar and zero-copy
span reads. Google Benchmark provides the local harness, but ordinary, package, install, and
export builds must not download or expose an additional dependency. Hosted CI is unsuitable for
absolute performance gating because its CPU allocation and load are variable.

## Decision

We will make Google Benchmark an opt-in development dependency behind
`SITOS_BUILD_BENCHMARKS`, defaulting to OFF. The pinned version and SHA-256 are fetched only for
that enabled build; benchmark tests and installation are disabled, and benchmark targets are
excluded from ordinary, package, install, and export builds.

## Consequences

* Good: N01 measurements use a repeatable public-API harness without adding a runtime or
  installed-package dependency.
* Good: dependency integrity is checked by the pinned version and SHA-256.
* Good: the local WSL2 Ubuntu 24.04 Release procedure records five one-second runs, aggregate
  medians, and the `<1,000 ns/op` and `<=4x` direct-lookup acceptance bounds.
* Bad: developers must explicitly enable the option and provide network access for the first
  benchmark dependency fetch.
* Neutral: Issue #33 owns stable-runner selection, historical baselines, and any future
  performance-regression gating; hosted CI execution remains nonblocking.

## Options Considered

* **Fetch Google Benchmark for every build** — rejected because ordinary and package consumers
  must not acquire an unrelated dependency or require network access.
* **Vendor or install Google Benchmark with sitos** — rejected because it widens the installed
  package dependency surface without benefiting library consumers.
* **Use hosted CI as an absolute benchmark gate** — rejected because runner variance makes
  nanosecond thresholds unreliable.

## References

* Issue #19 / PR #102
* Issue #33
* [Dependency policy](../09_dependency_policy.md)
* [ADR process](../10_adr_process.md)
