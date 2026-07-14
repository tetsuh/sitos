# ADR-0018: Use a zenoh-valid batch key segment

## Status

Proposed — 2026-07-15

## Context

The original batch wire path reserved the terminal segment `$batch`. During
Issue #13's same-session zenoh-c 1.9.0 RED phase, `Transport::Put(.../$batch, ...)`
failed while an ordinary sibling key on the same transport succeeded.
`z_keyexpr_from_str_autocanonize` also rejects it: `$` participates in zenoh's
`$*` wildcard syntax. The first replacement candidate, `@batch`, could be put
but neither `<prefix>/**` nor `session/<sid>/**` received it, so StorageNode
never applied the sample.

Focused same-session probes then verified that `~batch`, `%batch`, `+batch`,
`:batch`, and `!batch` are valid zenoh-c key expressions and are delivered
through a normal `/**` subscription. A batch path must also be outside the sitos
user-key grammar and communicate that it is a reserved control operation.

## Decision

We will reserve `:batch` as the terminal batch segment. Batch puts use
`<prefix>/base/:batch` and `<prefix>/session/<sid>/:batch`. The colon marks a
named control operation and is outside the sitos user-key grammar. No `$batch`,
`@batch`, or `~batch` compatibility alias is provided because no v0.2 batch
wire was released.

## Consequences

* Good: batch paths are valid zenoh-c 1.9.0 key expressions and remain outside
  the user-key grammar.
* Good: normal `base/**` and `session/<sid>/**` subscriptions receive the
  original batch sample without a separate declaration.
* Good: `:batch` reads as a named control segment rather than application data.
* Bad: pre-release documentation and fixtures using `$batch`, `@batch`, or
  `~batch` must be updated.
* Neutral: retired candidates remain invalid sitos batch keys and cause zero
  StorageNode mutation.

## Options Considered

* **Keep `$batch` with escaping or unchecked Zenoh APIs** — rejected because
  zenoh-c rejects it as a key expression and unchecked construction would evade
  transport validation.
* **Use `@batch`** — rejected because it does not intersect the normal zenoh
  wildcard subscriptions required for StorageNode and ParamCache delivery.
* **Use `~batch`** — technically valid and wildcard-deliverable, but rejected
  because its home-directory and approximation associations do not clearly
  identify a control operation.
* **Use `%batch`, `+batch`, or `!batch`** — technically valid and
  wildcard-deliverable, but rejected to avoid percent-encoding, form/URL, and
  shell/YAML punctuation concerns.
* **Use an ordinary user-key segment** — rejected because it could collide with
  application data.

## References

* Issue #13
* `BatchIntegrationTest.BatchIsReceivedBySessionSubscriber` proves `:batch`
  reaches `session/<sid>/**` and is applied by StorageNode on zenoh-c 1.9.0.
* [Wire protocol](../03_wire_protocol.md)
* [ADR process](../10_adr_process.md)
