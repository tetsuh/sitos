# ADR-0007: Adopt legacy-compatible payload v1 with Encoding-based versioning

## Status

Accepted — 2026-07-07

## Context

sitos must be able to exchange parameter values with existing systems that use
a specific byte layout (one-byte type tag followed by little-endian data). At
the same time, the wire format must be versioned so that future formats such
as CBOR can be introduced without breaking interoperability.

## Decision

We will adopt the legacy-compatible byte layout as payload v1 and register it
under the zenoh Encoding schema name `sitos.v1`. Future formats will use
separate schema names.

## Consequences

* Good: Migration from existing systems is straightforward.
* Good: Encoding-based versioning keeps wire-format selection explicit and
  extensible.
* Bad: The v1 format is less compact than some alternatives; future schemas
  can address this.

## Options Considered

* **CBOR only** — rejected because it would force an immediate migration of
  existing data producers.
* **Legacy layout with no versioning** — rejected because it would lock the
  project into one format forever.
* **Legacy layout as `sitos.v1` with Encoding-based versioning** — selected for
  compatibility and future extensibility.

## References

* Issue #1
* Related: ADR-0001
