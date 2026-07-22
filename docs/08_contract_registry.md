# sitos — Public Contract Registry

An index of every wire surface and stable cross-component identifier in sitos. The registry exists
so that a second surface with an overlapping purpose cannot be introduced silently: any issue that
adds or changes a contract must reference its row here, and overlap forces an explicit decision.

**The registry is an index, not a specification.** Contract text lives only in the linked normative
documents; rows never restate byte layouts, grammars, or values, so the registry cannot diverge
from the specifications it points to.

## 1. Rules

1. An issue that **adds or changes** a wire surface or stable identifier must reference the
   affected registry row in its body, and the implementing PR must update that row in the same PR.
2. Introducing a **second surface whose purpose overlaps an existing row** requires an ADR that
   records why the existing surface cannot be reused (see [10_adr_process.md](10_adr_process.md)).
3. **Planned** rows name their design authority (a proposal issue or ADR). Mechanism details in
   planned rows and their referenced sections are tentative until that authority's ADR is
   Accepted. Forward-written specification sections carry the banner described in
   [development_workflow.md](development_workflow.md) §7.4.
4. When a milestone is assembled, the new and changed surfaces of its issues are checked against
   this registry as part of the milestone design review
   ([development_workflow.md](development_workflow.md) §7).

## 2. Wire surfaces

Everything observable by another process over zenoh.

| Surface | Status | Normative spec | Design authority / implementer |
|---|---|---|---|
| Key space and keyexpr grammar (`base/`, `session/<sid>/`, `snap/<sid>/`, `:batch` segment, `meta/**`; id grammar for `<sid>`/`<uuid>`) | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) §1 | `src/key.cpp` |
| Payload v1 (single value: type tag + LE body, canonical NaN; golden fixtures `tests/fixtures/payload_v1/`) | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) §2 | `ParamValue` codec |
| Batch v1 (`:batch` multi-entry payload) | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) §5 | batch codec |
| Encoding identifiers (`Encoding::kSitosV1`, `Encoding::kSitosV1Batch`) | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) | `include/sitos/transport.hpp` |
| `meta/session/<sid>` reply (session metadata JSON) | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) §7 | StorageNode meta route |
| `meta/ack/<uuid>` route, ack token attachment, and AckResult payload | **Planned, not normative** | [03_wire_protocol.md](03_wire_protocol.md) §6 | Proposal #114 (pending) → ADR; implementers #14/#17 |
| Same-publisher in-band fence marker | **Planned, not normative** | — (to be added on ADR acceptance) | #106 ADR, shared result protocol per #114; consumers #99/#107 |
| `buffers/<sid>/**` value scope (opaque binary values) | **Planned, not normative** | [ADR-0014](adr/0014-session-scoped-buffers.md) | #56; fences via #107 |

## 3. Stable identifiers

Values that must remain stable across releases because callers persist, compare, or match on them.

| Identifier set | Status | Normative spec | Stability rule |
|---|---|---|---|
| `Status` enum numeric values | Normative, implemented | [04_api_cpp.md](04_api_cpp.md) §1.1 | Append-only; existing values are never renumbered. Planned append: `OutcomeUnknown` (#114) |
| Python exception hierarchy (`sitos.SitosError` and subclasses, Status mapping) | Normative, implemented | [05_api_python.md](05_api_python.md) §2.1 | One registered class per name; mapping extends only when `Status` extends. Planned append: `OutcomeUnknownError` (#114) |
| Session id / ack-uuid grammar | Normative, implemented | [03_wire_protocol.md](03_wire_protocol.md) §1 | Canonical UUID forms are subsets; grammar is never narrowed |

(END OF DOCUMENT)
