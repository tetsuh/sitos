# ADR-0019: Additive client result and configuration foundation

- Status: Proposed
- Date: 2026-07-16

## Context

Higher-level synchronous clients need stable status classification, diagnostic
messages, native error causes, and shared connection configuration. The error
state of the existing implemented `Result<T>` only carried a `std::error_code`
and is already used by the transport and storage APIs, so replacing it would
break source compatibility.

## Decision

Add a public `Status` enumeration and status error category, and extend
`Result<T>` additively while preserving its existing factories, state queries,
value accessors, and native `Error()` cause. Results use exclusive value/error
states. New errors carry `ErrorInfo{Status, message, cause}`; `ErrFrom` copies
error information between result value types without requiring the source
value to be copyable. Legacy error-code construction uses the closed portable
`std::errc` mapping defined in the public result contract.

Add dependency-free `ClientConfig` and `ValidateClientConfig`. Prefix grammar,
positive query timeout, and empty optional JSON configuration are validated
before transport work. Nonempty JSON5 is parsed as a complete Zenoh
configuration by the transport adapter. The public `OpenZenohTransport`
factory returns a status-bearing result, while `MakeZenohTransport` remains a
lossy nullptr-returning compatibility wrapper.

The public umbrella header contains the existing dependency-free API. Library
sources continue to include direct headers, and no raw Zenoh header is exposed
outside `src/transport/`.

## Consequences

- Existing transport and storage callers continue to compile and retain native
  error-code comparisons.
- ParamStore and ParamCache can share configuration and status semantics.
- The synchronous Get completion and callback lifetime contract remains a
  separate decision in ADR-0020.
- This ADR must be accepted after implementation review and before merging the
  issue.
