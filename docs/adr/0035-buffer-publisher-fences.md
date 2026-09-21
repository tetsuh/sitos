# ADR-0035: Define BufferPublisher fence discovery and public mapping

## Status

Proposed — 2026-09-21

## Context

Issue #107 adds the public C++ and Python BufferPublisher over the existing buffer routes and
ADR-0029 same-publisher Fence primitive. A publisher must bind one active Session incarnation
without adding a second discovery route or a public session-binding object. The public applied and
synced receipt mapping must also consume ADR-0028's existing Fence result and ADR-0034's durability
capability rather than defining another wire result.

## Decision

We will add `generation_uuid`, as canonical lowercase UUIDv4 text, to the existing
`meta/session/<sid>` JSON payload. BufferPublisher opening queries that exact key with
`ClientConfig::query_timeout`, binds the returned generation immutably, and maps the existing
Fence result to `FenceReceipt` and `FenceDurability`; a missing reply is `NotFound`, malformed
encoding/payload/shape or generation is `TypeMismatch`, and transport statuses are preserved.

## Consequences

* Good: same-SID recreation receives a fresh generation and old publishers fail closed under
  ADR-0029 without rebinding.
* Good: C++ and Python expose one explicit, parity-preserving receipt mapping over the existing
  marker and acknowledgement contracts.
* Bad: metadata payloads gain one backward-compatible wire-v1 field and older clients cannot
  perform generation-bound BufferPublisher discovery against a node that omits it.
* Neutral: ephemeral publishers support only applied fences; synced fences require the durable
  capability from ADR-0034.

## Options Considered

* **Existing session metadata** — selected because it preserves the frozen Open API and avoids a
  second discovery route.
* **A new generation-discovery route** — rejected because it duplicates session metadata purpose.
* **Test-only UUID injection or a public binding object** — rejected because neither is part of
  the approved Issue #107 scope.

## References

* Issue #107 and owner decision comment `issuecomment-5756529228`
* ADR-0028 (acknowledged operation results)
* ADR-0029 (same-publisher Fence ordering)
* ADR-0034 (storage durability barrier)
* [03] §6.1 and §7.1; [04] BufferPublisher API; [05] BufferPublisher API
