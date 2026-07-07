# ADR-0003: Ship InMemory and RocksDB engines; do not adopt LevelDB

## Status

Accepted — 2026-07-07

## Context

sitos needs at least one zero-dependency storage engine for ease of use and
one persistent engine for production deployments. The legacy system used
LevelDB, which has become effectively frozen upstream (stagnant since v1.23
in 2024) and is therefore a poor long-term dependency for a public OSS
project.

## Decision

We will ship an InMemory engine as the default and a RocksDB engine as an
opt-in component. We will not adopt LevelDB.

## Consequences

* Good: InMemory requires no external dependencies, simplifying development
  and testing.
* Good: RocksDB is actively maintained and widely packaged.
* Good: Avoiding LevelDB removes a dependency with an uncertain future.
* Neutral: Existing LevelDB-based data must be migrated through an external
  adapter rather than by sitos itself.

## Options Considered

* **LevelDB** — rejected because upstream development has stalled and it is
  unsuitable as a bundled dependency for public OSS.
* **RocksDB** — selected for persistence because it is actively maintained and
  license-compatible.
* **InMemory** — selected as the default because it has zero dependencies and
  covers testing and lightweight deployments.

## References

* Issue #1
* Related: ADR-0002
