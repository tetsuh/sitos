# ADR-0021: Resolve installed Zenoh dependencies without fetching or bundling

## Status

Proposed — 2026-07-18

## Context

The installed static `sitos::sitos` target retains a link-only dependency on the
shared `zenohc::zenohc` target when built with Zenoh enabled. The current build
fetches zenoh-c into the build tree and does not install a package config for
that dependency, so a downstream `find_package(sitos)` cannot reconstruct the
imported target. Downstream package discovery must remain relocatable and must
not perform hidden network access.

## Decision

We will install a `Findzenohc.cmake` module with the sitos package and use it to
recreate or reuse an externally provisioned `zenohc::zenohc` target for Zenoh-ON
packages. Zenoh-OFF packages have no Zenoh dependency; package discovery never
fetches or bundles zenoh-c.

## Consequences

* Good: Installed packages are relocatable and have no hidden configure-time
  network access.
* Good: Static sitos consumers reconstruct the required shared zenoh-c target
  before loading the exported sitos target.
* Bad: Zenoh-ON consumers must provide zenoh-c through `zenohc_ROOT`,
  `ZENOHC_ROOT`, or a normal CMake prefix when it is not installed in a standard
  location.
* Bad: The application or package manager owns deployment of `zenohc.dll` or
  `libzenohc.so` and the corresponding runtime search-path configuration.

## Options Considered

* **Fetch zenoh-c from the installed package config** — rejected because package
  discovery would require network access and would not be deterministic.
* **Bundle zenoh-c in the sitos install tree** — rejected because the dependency
  is a separately versioned shared runtime and bundling would create a new
  redistribution and runtime deployment policy.
* **Require consumers to create `zenohc::zenohc` manually** — rejected because
  the exported sitos target would otherwise fail with an undocumented target
  error during downstream CMake generation.

## References

* Issue #96
* PR implementing Issue #96
* [Dependency policy](../09_dependency_policy.md)
