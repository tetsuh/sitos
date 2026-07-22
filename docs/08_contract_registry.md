# sitos — Public Contract Registry

An index of every wire surface and stable cross-component identifier in sitos. The registry exists
so that a second surface with an overlapping purpose cannot be introduced silently: any issue that
adds or changes a contract references its row here, and overlap forces an explicit decision.

Each contract is tracked along three independent axes, because they move separately:

* **Contract status** — `Normative` once a specification or an Accepted ADR fixes it, otherwise
  `Planned`.
* **Implementation status** — `Implemented` once merged code realizes it, otherwise `Planned`.
  A contract can be `Normative` while its implementation is still `Planned` (an Accepted ADR whose
  code has not landed), and vice versa (a merged de-facto behavior not yet documented normatively).
* **Design authority** — the single Issue or ADR that owns the decision, or `—` once the contract is
  settled and normative. Implementers and downstream consumers are recorded separately; they are not
  the design authority.

Row cells are **non-normative summaries**; the linked specifications are authoritative. The registry
owns inventory, contract maturity, implementation status, and design authority — not contract text —
so it cannot diverge from the specifications it points to.

## 1. Rules

1. A new wire surface or stable identifier is added here as a **Planned row during issue or proposal
   review, or at milestone assembly — before implementation begins.** An issue that adds or changes
   a contract references its row (existing, or the Planned row added at review time). The
   implementing PR **advances** the row's Implementation status; the PR that lands the owning ADR
   advances its Contract status. Neither performs first registration.
2. Introducing a **second surface whose purpose overlaps an existing row** requires an ADR that
   records why the existing surface cannot be reused (see [10_adr_process.md](10_adr_process.md) §6).
3. A Planned row names **exactly one** design authority and always carries an open owning Issue or
   ADR, so it cannot remain tentative without a tracked owner. A proposal Issue must hand off to an
   ADR: the row's **Contract status becomes `Normative` only when that owning ADR is Accepted** — a
   proposal Issue alone never makes a row normative. Forward-written specification sections carry the
   banner described in [development_workflow.md](development_workflow.md) §7.4 until that event.
4. When a milestone is assembled, the new and changed surfaces of its issues are checked against
   this registry as part of the milestone design review
   ([development_workflow.md](development_workflow.md) §7); any unregistered surface is added as a
   Planned row during that gate.

## 2. Wire surfaces

Everything observable by another process over zenoh. "Design authority" is the single owner of an
open decision (`—` when settled); implementers and consumers are listed separately.

| Surface | Contract | Implementation | Normative spec | Design authority | Implementer / consumers |
|---|---|---|---|---|---|
| Key space and keyexpr **path grammar** (`base/`, `session/<sid>/`, `snap/<sid>/`, `:batch` segment, `meta/**` route shapes) | Normative | Implemented | [03](03_wire_protocol.md) §1 | — | `src/key.cpp` |
| Operation-to-key mapping (Put/Get/List/Delete → key expressions) | Normative | Implemented | [03](03_wire_protocol.md) §3 | — | `src/key.cpp`, ParamStore |
| Query semantics (single-key get, List enumeration, zero-reply, wildcard, read-only snap/session, unknown session) | Normative | Implemented | [03](03_wire_protocol.md) §4 | — | StorageNode routing |
| Payload v1 (single value: type tag + LE body, canonical NaN; golden fixtures `tests/fixtures/payload_v1/`) | Normative | Implemented | [03](03_wire_protocol.md) §2.1 | — | `ParamValue` codec |
| zenoh Encoding identifiers and normalization (`kSitosV1`, `kSitosV1Batch`, legacy spelling, absent/unknown fallback) | Normative | Implemented | [03](03_wire_protocol.md) §2.2 | — | `include/sitos/transport.hpp` |
| Batch v1 (`:batch` multi-entry payload) | Normative | Implemented | [03](03_wire_protocol.md) §5 | — | batch codec |
| `meta/session/<sid>` reply (session metadata JSON) | Normative | Implemented | [03](03_wire_protocol.md) §7.1 | — | StorageNode meta route |
| `meta/ack/<uuid>` **route behavior** (token recording, AckResult payload, query semantics) | Planned | Planned | [03](03_wire_protocol.md) §6 (outline) | #14 → ADR | #17 (ParamStore policy); #114 pending consolidation |
| Same-publisher in-band fence marker | Planned | Planned | — (added on ADR acceptance) | #106 → ADR | consumers #99, #107 |
| `buffers/<sid>/**` value scope (opaque binary values) | Normative | Planned | [ADR-0014](adr/0014-session-scoped-buffers.md) | — | #56; fences via #107 |

## 3. Stable identifiers

Values that must remain stable across releases because callers persist, compare, or match on them.

| Identifier set | Contract | Implementation | Normative spec | Design authority | Stability rule / notes |
|---|---|---|---|---|---|
| `Status` enum numeric values (`Ok`..`Error`) | Normative | Implemented | [04](04_api_cpp.md) §1.1 | — | Append-only; existing values are never renumbered |
| Planned `Status` append: `OutcomeUnknown` | Planned | Planned | — | #114 → ADR | |
| Python exception hierarchy (`sitos.SitosError` and current subclasses, Status mapping) | Normative | Implemented | [05](05_api_python.md) §2.1 | — | One registered class per name; mapping extends only when `Status` extends |
| Planned Python exception: `OutcomeUnknownError` | Planned | Planned | — | #114 → ADR | |
| Session-id grammar (`<sid>`) | Normative | Implemented | [03](03_wire_protocol.md) §1 | — | `src/key.cpp` `IsValidSessionId`; grammar is never narrowed |
| `meta/ack/<uuid>` route id grammar (lenient parser, `IsValidAckUuid`) | Planned | Implemented | not yet documented in [03](03_wire_protocol.md) §1 | #14 → ADR | parser-accepted de-facto grammar; to be documented normatively; #114 pending consolidation |
| Generated correlation-id format (canonical UUIDv4) | Planned | Planned | — | #114 → ADR | |
| Fence result identifiers (`FenceDurability`, `FenceReceipt` fields) | Planned | Planned | — | #107 → ADR | reuses the #106/#114 result protocol |
| Session state-lost read result (overlay/snapshot lost after restart) | Planned | Planned | — | #108 → ADR | |
| Typed catalog-unavailable result (catalog corruption fail-closed) | Planned | Planned | — | #108 → ADR | |

(END OF DOCUMENT)
