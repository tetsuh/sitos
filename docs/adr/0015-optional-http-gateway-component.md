# ADR-0015: Ship an optional HTTP gateway component on cpp-httplib

## Status

Superseded by ADR-0027 — 2026-07-21

## Context

sitos exposes parameters, sessions, and buffers over zenoh. Browsers and
generic HTTP tooling cannot speak zenoh, and operators frequently want to
inspect or drive a store from a browser or `curl` for debugging. zenoh ships a
generic REST plugin, but it requires a router daemon, is untyped with respect to
the `sitos.v1` payload, and is unaware of sessions, snapshots, and buffers.

sitos follows a minimal-dependency policy (`docs/09_dependency_policy.md`): the
core must not gain a heavy HTTP framework dependency. A gateway is therefore an
**optional** component, in the same spirit as the opt-in RocksDB engine
(ADR-0003).

## Decision

We will ship an optional `sitos-gateway` component, gated by a CMake option
(`SITOS_BUILD_GATEWAY`, default OFF) and built on **cpp-httplib**
(header-only, MIT). It exposes params/sessions/buffers over HTTP with
Server-Sent Events for subscriptions, understands the `sitos.v1` payload types,
binds to loopback by default with pluggable authentication, and publishes a
`Router` extension point so a host application can register additional routes on
the same server.

## Consequences

* Good: Browser and `curl` access to a sitos store without a zenoh client and
  without a router daemon.
* Good: Typed over `sitos.v1` (Bool/S64/Dp/Str/Bytes), session- and
  buffers-aware, with SSE for delta subscription — value beyond the generic
  zenoh REST plugin.
* Good: The core keeps zero HTTP dependencies; cpp-httplib is pulled only when
  `SITOS_BUILD_GATEWAY=ON`, exactly like the RocksDB engine.
* Good: The `Router` extension point lets host applications mount their own
  routes on one server/port (one origin), so they do not need a second HTTP
  stack.
* Neutral: cpp-httplib is thread-per-connection and has no WebSocket; SSE covers
  the subscription use case. Binary WebSocket push, if ever needed, is a
  separate follow-up.
* Neutral: Positioned as a gateway/debug surface. It is not a hardened public
  endpoint; deployments must add authentication and network controls.
* Trade-off: A new optional SOUP (cpp-httplib) enters the build matrix and must
  be tracked with locked-version + latest CI, like RocksDB and Python.

## Design notes (normative for the implementation)

* Component: `sitos-gateway` library + a thin standalone binary. The library
  form is what host applications embed.
* Dependency: cpp-httplib, optional, `SITOS_BUILD_GATEWAY` gated. TLS is
  provided by cpp-httplib's OpenSSL backend only when explicitly enabled.
* Client: the gateway is a sitos client and depends on ParamStore / ParamCache /
  SessionController, so it lands after those exist.
* Surface (indicative): GET/PUT a parameter, GET/List parameters, session
  create/close/list, GET a buffer, and an SSE stream for session deltas. Typed
  values map to/from `sitos.v1`. For `kEphemeral` buffer sessions (ADR-0014),
  GET returns not-found by design; the mode is surfaced in session metadata.
* Security: default bind `127.0.0.1`; authentication is a pluggable hook, not a
  built-in identity system.
* Extension point: `Router& router()` (or equivalent) so host code can register
  additional routes handled in-process. The gateway itself contains no
  application-specific control logic.
* API stability and threading are validated with locked-version + latest CI, per
  `docs/09_dependency_policy.md`.

## Options Considered

* **zenoh REST plugin only** — rejected: requires a router daemon, is untyped
  over `sitos.v1`, and is unaware of sessions/snapshots/buffers.
* **Poco or a full HTTP framework in the core** — rejected: violates the
  minimal-dependency policy; a heavy always-on dependency for an optional
  feature.
* **A separate standalone gateway process only (no embeddable library)** —
  rejected: host applications that want one server/origin would need a second
  HTTP stack or a reverse proxy.
* **Optional `sitos-gateway` library on cpp-httplib with a `Router` extension
  point** — selected: minimal optional dependency, embeddable, one origin,
  SSE-capable.

## References

* Related: ADR-0003 (optional component precedent), ADR-0006 (C++20 core),
  ADR-0007 (`sitos.v1` payload), ADR-0014 (buffers)
* Docs: `docs/09_dependency_policy.md`
* Issue: #57 (implementation)
