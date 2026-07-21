# ADR-0027: Keep HTTP control planes in host applications

## Status

Proposed — 2026-07-21

## Context

sitos was previously expected to ship an optional HTTP gateway under ADR-0015.
The initial production integration instead has a host orchestrator that already
owns HTTP routing, authorization, access policy, audit, and lifecycle control.
A second sitos-owned HTTP surface would duplicate those responsibilities and
create an independent security, dependency, CI, and compatibility boundary
without an identified production consumer.

## Decision

We will not ship `sitos-gateway` or the `SITOS_BUILD_GATEWAY` build option.
Host applications and orchestrators will provide HTTP control planes by calling
the native sitos APIs; sitometron, the external host orchestrator application,
is the initial host implementation. Any future
standalone gateway requires concrete demand and a new ADR and may be delivered
as a separate package or repository.

## Consequences

* Good: sitos remains focused on its Zenoh-based parameter, session, and buffer
  data plane.
* Good: host authorization, audit, and lifecycle policy cannot be bypassed by a
  parallel sitos-owned HTTP surface.
* Good: cpp-httplib and gateway-specific build, security, and compatibility
  maintenance are removed from the planned sitos scope.
* Neutral: hosts that need HTTP access must provide a facade over the native
  C++ or Python APIs.
* Neutral: native APIs, Zenoh tooling, and the `sitobolon` development
  executable remain available for inspection and integration.
* Trade-off: sitos does not provide a standalone browser or `curl` endpoint.

## Options Considered

* **Keep the optional in-repository gateway from ADR-0015** — rejected because
  it duplicates the host control plane and has no identified production
  consumer.
* **Ship only a standalone gateway process** — rejected because it still adds a
  second policy and maintenance boundary.
* **Let host applications own HTTP and reconsider a separate package later** —
  selected because it keeps policy in one place and preserves a future option
  when concrete demand exists.

## References

* Issue #109
* Issue #57 and its
  [architecture-decision timeline comment](https://github.com/tetsuh/sitos/issues/57#issuecomment-5024834430)
* Proposed replacement for: ADR-0015
* Related: ADR-0014
