# sitos — ADR Process

Rules for writing and operating ADRs (Architecture Decision Records) in the
sitos repository. Public document. In issue #1, this will be moved as
`docs/adr/README.md`.

## 1. Purpose

* Make it possible for future contributors (both humans and AI) to trace “why
  this design was chosen”
* Prevent rehashing discussions (record rejected options and the reasons)
* Serve as the source of truth for instructions to implementation AI

## 2. Placement and Naming

```
docs/adr/
  README.md                     # These rules (English)
  template.md                   # Template (§5)
  0001-use-zenoh-as-transport.md
  0002-embedded-storage-node.md
  ...
```

* File name: `NNNN-short-kebab-case-title.md` (4-digit zero-padded sequential number)
* Gaps in numbers are allowed (rejected proposals also consume numbers)
* One decision per file. Do not mix multiple independent decisions into one ADR

## 3. Format: MADR-lite

Use the following six-section structure, a simplified form of
[MADR](https://adr.github.io/madr/).

| Section | Required | Contents |
|---|---|---|
| Status | ✔ | Status from §4 + decision date |
| Context | ✔ | Background and constraints that made the decision necessary. 3–10 lines |
| Decision | ✔ | Decision content. 1–3 sentences in the active form “We will ...” |
| Consequences | ✔ | Positive effects, negative effects, and accepted trade-offs |
| Options Considered | Recommended | Alternatives considered and reasons for rejection (1–3 lines each) |
| References | Optional | Related issues/PRs, external materials, related ADRs |

## 4. Status Transitions

```
Proposed ──► Accepted ──► Deprecated
                │              ▲
                └── Superseded by ADR-NNNN
Proposed ──► Rejected
```

* **Proposed**: Proposed in a PR
* **Accepted**: Merged = an effective decision
* **Rejected**: Rejected (kept as a record)
* **Superseded by ADR-NNNN**: Replaced by a new ADR. Do not edit the old ADR;
  update only the Status line (history must not be rewritten)
* **Deprecated**: The target feature itself has been discontinued

**Invariant**: Do not rewrite the Context/Decision/Consequences of an ADR after
it becomes Accepted. If you want to change the decision, file a new ADR.
Only typo fixes and broken-link fixes are allowed.

## 5. Template

```markdown
# ADR-NNNN: <Title in imperative form>

## Status

Accepted — 2026-07-07

## Context

<Why is this decision needed? What constraints apply?>

## Decision

We will <decision>.

## Consequences

* Good: <positive outcomes>
* Bad: <negative outcomes / accepted trade-offs>
* Neutral: <side effects worth noting>

## Options Considered

* **<Option A>** — rejected because <reason>
* **<Option B>** — rejected because <reason>

## References

* Issue #NN / PR #NN
* Related: ADR-NNNN
```

## 6. When to Write an ADR

An ADR is required for changes that fall under any of the following:

* Breaking changes to the public API, or changes to the wire protocol (`sitos.v1*`)
* Addition, removal, or major update of dependency libraries (zenoh, RocksDB,
  nanobind, etc.)
* Changes touching the invariants in [09_dependency_policy.md](09_dependency_policy.md) §6
* Changes to the thread model, consistency model, or snapshot semantics
* Changes to the build system or supported platforms
* Introducing a second surface that overlaps an existing contract-registry row
  (registry Rule 2, [08_contract_registry.md](08_contract_registry.md) §1)

ADRs are not needed for implementation details that do not fall under these
categories (internal refactoring, bug fixes, performance improvements).
When in doubt, discuss “whether an ADR is needed” in PR review using the
`needs-adr` label.

## 7. Review Flow

1. Create the ADR as `Proposed` and submit it as a **separate PR** from the
   implementation PR
   (small decisions may be bundled with the implementation PR. In that case,
   include `[ADR]` in the PR title)
2. Conduct review and discussion on the PR (the discussion record remains in the PR)
3. At merge time, update Status to `Accepted` + decision date
4. When implementation AI performs work, always include related ADRs in the
   prompt context

## 8. Initial ADR Set

At repository creation (issue #1), file D1 through D13 from
[00_overview.md](00_overview.md) §6 as the following individual ADRs in English
(Status: Accepted). After that, maintain the table in 00_overview §6 as an
index to the ADRs.

| ADR | Origin | Proposed title |
|---|---|---|
| 0001 | D1 | Use zenoh as the transport layer |
| 0002 | D2 | Implement an embedded storage node instead of zenoh storage-manager |
| 0003 | D3 | Ship InMemory and RocksDB engines; do not adopt LevelDB |
| 0004 | D4 | Expose engine-native snapshots through the zenoh key space |
| 0005 | D5 | Name the project sitos |
| 0006 | D6 | C++20 core with Python bindings |
| 0007 | D7 | Adopt legacy-compatible payload v1 with Encoding-based versioning |
| 0008 | D8 | License under Apache-2.0 |
| 0009 | D9 | English as the repository language |
| 0010 | D10 | Use nanobind for Python bindings |
| 0011 | D11 | Develop in public from day one |
| 0012 | D12 | Google C++ style with 100-column limit |
| 0013 | D13 | Default to zenoh scouting with explicit endpoint override |

(END OF DOCUMENT)
