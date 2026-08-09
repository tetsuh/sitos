# Contributing to sitos

This is the canonical development workflow for sitos. It defines the branching strategy,
ticket-driven development (TiDD), and test-driven development (TDD).

Because implementation is assumed to be performed by AI coding agents,
**everything is stated explicitly instead of relying on implicit conventions**.

## 1. Branching Strategy: trunk-based

```
main ─────●───────●───────●──────●──► (always releasable)
           \     /  \     /
            feat/3-param-value
                     feat/6-in-memory-engine
```

* **main is always green** (required CI paths). Direct pushes are prohibited;
  changes go only through PRs. Every PR head must be green before merge.
  Intentional RED commits may be created only on `feat/` branches and must be followed by GREEN
  before merge; every merge commit on `main` is green
* Working branches are **short-lived** (guideline: 1 issue = 1 branch = 1 PR,
  within a few days)
* Branch name: `feat/<number>-<short-kebab-description>`
  (example: `feat/3-param-value-codec`). The prefix is always `feat/` regardless
  of change type; the change type lives on the issue label and the commit
  message (§2.1), so the branch does not repeat it
* Releases are tags on main (`v0.1.0`, etc.). Do not create release branches
  until a hotfix is needed
* Do not create long-lived develop / feature branches
  (GitFlow is excessive for this project’s scale and team structure)

## 2. Ticket-Driven Development (TiDD)

**No Ticket, No Commit.** Every change starts from a GitHub Issue.

1. Before starting work, confirm that an issue exists
   (#1 through #23b in [docs/07_issue_breakdown.md](docs/07_issue_breakdown.md) are the initial set)
2. Include the issue number in the branch name and PR. Write `Closes #NN` in
   the PR body
3. Do not mix changes outside the issue scope into the PR.
   For problems found during work, **file a new issue** and handle them there
   (drive-by fixes are prohibited. However, trivial fixes such as typos may be
   allowed at the reviewer’s discretion)
4. Commit messages follow Conventional Commits + issue references (§2.1)

### 2.1 Commit Message Convention

**Structure**: line 1 = Conventional Commits header, line 2 = blank line,
line 3 onward = bullet-list body.

```
<type>(<scope>): <summary> (#<issue>)

- <change 1: what and why>
- <change 2>
- <additional notes if any (trade-offs, follow-up issues, etc.)>
```

Examples:

```
feat(codec): implement payload v1 encoder (#4)

- Add ParamValue::Encode() producing type-tag + LE byte layout
- Verify against golden fixtures in tests/fixtures/payload_v1/
- Reject payloads shorter than 1 byte with Status::Error

fix(cache): prevent missed puts during attach (#18)

- Buffer subscriber samples until initial fetch completes
- Add race-reproduction test AttachDoesNotMissConcurrentPut
```

Rules:

* **Header (line 1)**
  - type: `feat` / `fix` / `docs` / `test` / `refactor` / `build` / `ci` / `chore`
  - scope: component name (codec, engine, node, store, cache, transport,
    python, adr, ...). May be omitted if none applies
  - summary: English, imperative mood, starts with lowercase, no trailing period,
    within 72 characters. Append issue number `(#N)` to the end of the header
* **Body (line 3 onward)**
  - Write only bullet-list items beginning with `- ` (do not write prose paragraphs)
  - Each item includes “what changed” and, if non-obvious, “why”
  - One change per item. The body may be omitted for trivial commits (typos, etc.)
* **Footer (optional)**: For breaking changes, write
  `BREAKING CHANGE: <description>` after the body (release-please uses this for
  major version bumps)

Feature-branch commits follow this convention. The GitHub-generated merge commit is governed by
§4 and is not manually rewritten to fit the convention.

### 2.2 Issue Requirements (for AI Implementers)

Each issue must include at minimum the following (following the format in [07]):

* Reference documents (applicable sections of the design documents)
* Target implementation files
* Acceptance criteria (AC) — in a verifiable form
* Dependent issues
* Affected contract-registry row(s) with their status transition (`Contract` /
  `Implementation` / `none`), or `N/A` if no contract is touched
  ([docs/08_contract_registry.md](docs/08_contract_registry.md))

### 2.3 Issue Scope Lifecycle

* Design, Proposal, and ADR Issues may evolve during review until the owner explicitly declares
  their scope ready for implementation.
* An implementation Issue checklist freezes when implementation begins. The Issue remains the
  scope authority, and the frozen checklist is copied into the PR for progress tracking.
* After the freeze, wording may be clarified only when its meaning does not change.
* A material scope change requires an owner decision, renewed review, and an Issue split when
  needed. Work on affected scope pauses until those steps are complete.
* Scope readiness and freezing do not replace required ADR, Contract Registry, dependency, or
  milestone-review gates.

## 3. Test-Driven Development (TDD): Red-Green-Refactor

This project has predefined AC, required test names ([06] §5.1), and golden
fixtures ([03] §2.3), making it particularly compatible with TDD.

**Implementation steps for each issue (required):**

```
1. RED    — Write tests corresponding to the AC first and confirm that they fail
            (for items with golden fixtures, start with fixture verification tests)
2. GREEN  — Write the minimum implementation that makes the tests pass
3. REFACTOR — Improve the design while keeping the tests green
            (naming, duplicate removal, compliance with the style in [06] §2)
```

Rules:

* **Do not commit production code without tests**
  (exceptions: build settings, docs, examples)
* Record concise structured RED evidence in the PR description:
  - command;
  - failing test name;
  - expected failure reason;
  - one representative failure-message line;
  - complete CI-log link only when the excerpt is insufficient.
  If the test passes from the beginning, that is a sign that the test itself is wrong. For build,
  documentation, or example-only exceptions, record `N/A` with the reason instead of inventing RED
  evidence
* Use the fixed AC test names from [06] §5.1. Additional tests are unrestricted
* Do not change the meaning of tests during refactoring (weakening assertions is prohibited)
* For bug fixes, **write a reproduction test first** (RED) → fix (GREEN)

## 4. PR Rules

* 1 PR = 1 issue. Keep diffs small (guideline: 500 lines or less for
  implementation + tests. If larger, consider splitting the issue)
* Required items in the PR template:
  - `Closes #NN`
  - Copy of the frozen Issue checklist
  - Corresponding requirement IDs ([01] F/N/C/P/X)
  - AC verification results (test execution logs)
  - Structured RED-phase evidence (§3)
  - Judgment on whether an ADR is needed (whether [10] §6 applies; §6 now includes
    the contract-registry Rule 2 overlap trigger)
  - Affected contract-registry row(s) with their status transition (`Contract` /
    `Implementation` / `none`), or `N/A` if no contract is touched ([08])
  - Current-head owner merge authorization record, completed only after authorization is granted
* CI (build + all tests + clang-format + clang-tidy) must be green
* A normal merge commit is the default merge method, preserving intentional RED/GREEN/REFACTOR
  history from the feature branch while the PR head and `main` remain green
* The owner may choose squash merge when the branch-level history is not worth retaining
* Keep GitHub's generated default merge commit message; the owner does not edit it manually
* Rebase merge and auto-merge are prohibited

### 4.1 Owner-Directed Merge Authorization

The repository owner makes every merge decision. A coding agent may execute a merge only after an
explicit owner instruction for that PR at its current head.

* Authorization is one-time and expires after any new commit or new blocking finding
* Passing CI or automated review never implies merge authorization
* Record the authorized head and owner-instruction link in the PR before merge
* Independent secondary review is optional and owner-directed; use it when its value justifies the
  available human time and AI subscription or token budget

## 5. Instruction Template for AI Implementers

Prompt structure when assigning an issue to implementation AI:

```
1. Full text of the target issue (reference documents, implementation targets, AC, dependencies)
2. Applicable sections of reference documents (entire sections)
3. Related ADRs
4. Instruction to comply with this workflow (§1–§4)
5. “First write the AC tests and confirm RED before implementing”
6. “Copy the issue checklist into the PR body and submit it with completed
   items checked”
```

The frozen checklist in the Issue is the **definitive definition of scope** under §2.3. Progress is
visualized using its copy in the PR body, and reviewers compare the PR check state with the
implementation artifacts. The Issue is closed by `Closes #NN` when the PR is merged (checkboxes on
the Issue side may remain unchecked when closed).

## 6. Release Flow

1. All required issues for the release boundary (the table at the beginning of
   [07]) are closed
2. release-please generates the CHANGELOG and version PR from Conventional Commits
3. Merge the version PR → tag → `wheels.yml` publishes to PyPI

## 7. Milestone Design Review (horizontal pass)

Per-issue reviews are depth-first; assembling a milestone additionally requires one breadth-first
pass (motivated by the Issue #114 retrospective: the ack and fence lanes were each reviewed
individually, and their shared substrate was found only after the milestone was assembled).

**Gate**: when a milestone is assembled or materially re-scoped, post one milestone design review
(a timeline comment on the milestone-defining issue, or a dedicated issue) covering §7.1–§7.4
before implementation of the milestone's issues begins. Guideline effort: half a day.

The gate **completes** only when its findings are recorded, each finding has a named follow-up
owner, and the milestone owner accepts the outcome — not merely when the artifact is posted. For a
**material re-scope**, rerun the gate before the added or changed scope begins and pause only the
affected work, not the whole milestone.

### 7.1 Shared-Mechanism Inventory

List the mechanisms each issue in the milestone needs (examples: tokens, correlation identifiers,
result reporting, polling/retry, ring buffers, ordering fences, catalogs). A **new, unresolved, or
materially changed** cross-component mechanism used by **two or more issues** and **lacking an
existing contract owner** gets a unifying design issue created at assembly time — a shared substrate
is a planned artifact, not a later discovery. If an existing owner or Accepted ADR already governs
the mechanism (for example the shared Result/Status model or logging), reference it instead of
creating a redundant issue.

### 7.2 Contract-Surface Check

List every wire surface or stable identifier the milestone adds or changes, and check each against
the [contract registry](docs/08_contract_registry.md). Any surface that has no row yet is added as a
Planned row during this gate (registry Rule 1), so first registration happens here rather than in a
later implementation PR. An addition whose purpose overlaps an existing row requires an ADR
recording why the existing surface cannot be reused.

### 7.3 Dependency and Intake Annotations

* For each cross-issue dependency inside the milestone, record in one line **which shared
  substrate the dependency encodes** (before asking whether it can be cut).
* When a new downstream consumer motivates the milestone, pair the new issues with a re-read of
  existing backlog issues touching the same layers, and record the updates each needs.

### 7.4 “Planned, Not Normative” Banner

A specification section written before its mechanism is decided must open with a banner naming the
deciding authority, in the form:

> **Planned, not yet normative:** Issue/ADR #NN owns this mechanism. Implementers must not treat
> this outline as a finalized contract.

Load-bearing contracts (wire payload layouts, key grammar, stable enum values) are decided early
and written normatively; mechanism details (retry counts, cache policies, marker representations)
default to this banner and are finalized in the owning pre-implementation ADR. Existing documents
are converted opportunistically when next edited.

(END OF DOCUMENT)
