# ADR-0016: Use canonical zenoh bytes encodings

## Status

Accepted — 2026-07-12

## Context

ADR-0007 assigns the `sitos.v1` and `sitos.v1.batch` schema names to sitos
payloads. The wire protocol documented their zenoh Encoding type using the
legacy spelling `zenoh.bytes`, while zenoh-c 1.9.0 represents its standard
bytes Encoding as `zenoh/bytes`. Passing a schema name directly to
`z_encoding_from_str()` does not add the bytes type; it creates an Encoding
whose type is the schema name itself. The transport also hard-coded received
samples as `sitos.v1`, preventing validation of the actual wire metadata.

## Decision

We will transmit sitos payloads with zenoh-c's canonical `zenoh/bytes` type and
the `sitos.v1` or `sitos.v1.batch` schema. The transport-independent API will
continue to expose schema identifiers, and the zenoh adapter will construct,
inspect, and normalize the corresponding wire Encoding.

## Consequences

* Good: Raw zenoh clients can identify sitos values as bytes with an explicit
  version schema.
* Good: Put and query replies use the same zenoh-c standard Encoding
  construction instead of relying on an arbitrary string.
* Good: Received Encoding metadata can be tested from the actual sample rather
  than replaced with a constant.
* Bad: The canonical wire spelling changes from the documented
  `zenoh.bytes;<schema>` form to `zenoh/bytes;<schema>`.
* Bad: The zenoh adapter must translate between transport schema identifiers
  and wire Encoding strings.
* Neutral: Receivers normalize both the canonical and legacy bytes spellings
  for compatibility, while senders emit only the canonical spelling.

## Options Considered

* **Keep schema-only Encoding values** — rejected because `sitos.v1` becomes
  the Encoding type instead of a schema on the standard bytes type.
* **Transmit `zenoh.bytes;<schema>`** — rejected because zenoh-c 1.9.0's
  standard bytes Encoding uses the canonical `zenoh/bytes` spelling.
* **Expose full zenoh Encoding strings through `sitos::Encoding`** — rejected
  because it would leak zenoh-specific representation into the transport
  abstraction.

## References

* Issue #9 / PR #73
* Related: ADR-0001, ADR-0007
* zenoh-c 1.9.0: `z_encoding_zenoh_bytes()`,
  `z_encoding_set_schema_from_str()`, and `z_encoding_to_string()`
