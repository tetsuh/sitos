# ADR-0008: License under Apache-2.0

## Status

Accepted — 2026-07-07

## Context

sitos is a public OSS project that depends on zenoh (EPL-2.0 / Apache-2.0
dual-licensed) and may optionally link with RocksDB (Apache-2.0 / GPLv2
dual-licensed). The chosen license must be compatible with these dependencies
and permissive enough for both academic and commercial users.

## Decision

We will license sitos under the Apache License, Version 2.0.

## Consequences

* Good: Apache-2.0 is compatible with zenoh's Apache-2.0 option and with
  RocksDB's Apache-2.0 option.
* Good: It is a permissive, widely understood license suitable for public OSS.
* Neutral: Contributors must accept the license terms for submitted patches.

## Options Considered

* **GPLv3** — rejected because it would restrict downstream users and conflict
  with the goal of broad adoption.
* **MIT** — rejected because it lacks an explicit patent grant, which Apache-2.0
  provides.
* **Apache-2.0** — selected for permissiveness, patent grant, and compatibility.

## References

* Issue #1
