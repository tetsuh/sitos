# ADR-0017: Use atomic, quiescent StorageNode lifecycle transitions

## Status

Accepted — 2026-07-13

## Context

StorageNode declarations are public Transport operations and can fail independently.
A started node must never expose only one declaration, and callbacks already in flight
must finish before declarations are released. Start, Stop, and IsStarted are also
called concurrently by applications, while caller-owned transports remain external
objects with an independently managed lifetime.

## Decision

We will return `Result<Queryable>` and `Result<Subscription>` from declaration APIs,
stage both declarations in inactive shared state, and atomically activate only after
both succeed. We will serialize lifecycle transactions and use a mutex/condition
variable callback gate that rejects new callbacks and waits for in-flight callbacks
before undeclaration.

## Consequences

* Good: declaration errors preserve their error codes and partial starts roll back safely.
* Good: Stop provides a deterministic callback quiescence boundary.
* Bad: Start and Stop operations are serialized and a live StorageNode remains non-movable.
* Bad: samples and queries arriving while declarations are staged are rejected. Callers must
  treat a successful Start return as the readiness boundary and coordinate publishers when
  startup delivery must not be lost.
* Neutral: caller-supplied Transports remain externally owned and must outlive the node.

## Options Considered

* **Atomic active flag only** — rejected because it cannot wait for callbacks that already entered.
* **Empty declaration handles as failure indicators** — rejected because failure details are lost.
* **Movable live nodes** — rejected because moving handles, gates, and external lifetimes is
  error-prone.

## References

* Issue #11
* [Transport dependency policy](../09_dependency_policy.md)
* [ADR process](../10_adr_process.md)
