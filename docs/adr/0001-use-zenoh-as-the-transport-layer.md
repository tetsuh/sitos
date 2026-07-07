# ADR-0001: Use zenoh as the transport layer

## Status

Accepted — 2026-07-07

## Context

sitos needs a communication layer that can distribute typed parameters and
lookup tables to multiple compute processes. The legacy system used a custom
synchronization protocol over ZeroMQ and required significant maintenance.
A replacement must provide publish/subscribe, request/reply, and distributed
storage semantics without custom low-level networking code.

## Decision

We will use Eclipse zenoh as the transport layer for sitos.

## Consequences

* Good: zenoh unifies pub/sub, queries, and storage semantics in one protocol.
* Good: Existing zenoh clients in any language can interoperate with sitos
  without a sitos-specific library.
* Good: Routing, discovery, and reliability are handled by the zenoh project.
* Neutral: The project must track zenoh release cadence and API changes.

## Options Considered

* **Custom protocol over ZeroMQ** — rejected because it recreates the
  synchronization and discovery problems of the legacy system.
* **MQTT + separate RPC layer** — rejected because it would require stitching
  together two protocols and does not provide distributed queries natively.
* **zenoh** — selected because it combines pub/sub, query, and storage-like
  access patterns in a single, actively maintained open-source project.

## References

* Issue #1
* Related: ADR-0002, ADR-0013
