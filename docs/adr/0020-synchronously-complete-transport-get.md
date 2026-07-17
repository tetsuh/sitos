# ADR-0020: Synchronously complete Transport Get requests

## Status

Proposed — 2026-07-16

## Context

`Transport::Get` is the low-level read primitive for future synchronous
ParamStore and ParamCache operations. Returning after Zenoh query submission
left reply callbacks active after the caller returned, so callers could not
safely retain local collection state or distinguish terminal zero replies from
an in-progress query. The transport must retain its backend isolation while
providing a completion boundary that works in Zenoh-enabled and disabled builds.

## Decision

We will make a successful `Transport::Get` return only after Zenoh drops its
reply closure and all C reply callbacks that entered for the request have
finished. A strictly positive timeout bounds the Zenoh query collection window;
terminal zero replies are successful transport completion. The adapter uses
`LATEST` consolidation and a per-request first-observed-key fallback, serializes
one request's sinks without serializing other requests, and suppresses later
delivery after sink false or the first recorded failure.

## Consequences

* Good: callers can copy reply data during a sink and use it safely after Get
  returns, without callbacks accessing caller state afterward.
* Good: sink exceptions and native reply-processing errors are contained at the
  C ABI boundary, reported as Error after quiescence, and cannot become false
  zero-reply success.
* Good: independent Get, Put, and Delete requests remain concurrently usable.
* Bad: Get blocks through normal terminal closure even after a sink returns
  false, so low-level sinks must remain short.
* Bad: recursively calling blocking Get from its own low-level sink is
  unsupported; nonblocking Put and Delete remain permitted.
* Neutral: a zero-reply terminal completion is not Timeout. Higher-level
  ParamStore semantics decide whether it means NotFound or an empty List.

## Options Considered

* **Return after query submission** — rejected because callbacks can outlive
  caller-owned collection state and make synchronous clients unsafe.
* **Expose a future or waitable handle** — rejected for v0.2 because the
  existing Get signature can provide the required completion boundary directly.
* **Serialize all requests with a session mutex** — rejected because unrelated
  Gets and nonblocking writes must make independent progress.

## References

* Issue #86
* ADR-0019
* `include/sitos/transport.hpp`
