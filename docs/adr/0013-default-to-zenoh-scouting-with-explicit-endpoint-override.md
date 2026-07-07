# ADR-0013: Default to zenoh scouting with explicit endpoint override

## Status

Accepted — 2026-07-07

## Context

sitos must work out of the box in development environments where multicast is
available, while also supporting production environments such as medical
devices where multicast may be prohibited and explicit endpoints are required.

## Decision

By default, sitos will rely on zenoh scouting for peer discovery. Users can
override this by providing explicit endpoints through the zenoh Config.

## Consequences

* Good: Zero-configuration setup in typical development networks.
* Good: Works in restricted networks when endpoints are configured.
* Neutral: Users in restricted environments must supply configuration.

## Options Considered

* **Explicit endpoints only** — rejected because it adds friction to local
  development and testing.
* **Scouting only** — rejected because it fails in environments that block
  multicast.
* **Scouting default with explicit endpoint override** — selected for a balance
  between convenience and flexibility.

## References

* Issue #1
* Related: ADR-0001
