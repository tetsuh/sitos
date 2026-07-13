# sitos — Development Workflow

Operation rules for the branching strategy, ticket-driven development (TiDD),
and test-driven development (TDD). Public document. This is the English edition
maintained as `docs/development_workflow.md` (or CONTRIBUTING.md).

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
  changes go only through PRs
* Working branches are **short-lived** (guideline: 1 issue = 1 branch = 1 PR,
  within a few days)
* Branch name: `feat/<number>-<short-kebab-description>`
  (example: `feat/3-param-value-codec`)
* Releases are tags on main (`v0.1.0`, etc.). Do not create release branches
  until a hotfix is needed
* Do not create long-lived develop / feature branches
  (GitFlow is excessive for this project’s scale and team structure)

## 2. Ticket-Driven Development (TiDD)

**No Ticket, No Commit.** Every change starts from a GitHub Issue.

1. Before starting work, confirm that an issue exists
   (#1 through #23b in [07_issue_breakdown.md](07_issue_breakdown.md) are the initial set)
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

### Issue Requirements (for AI Implementers)

Each issue must include at minimum the following (following the format in [07]):

* Reference documents (applicable sections of the design documents)
* Target implementation files
* Acceptance criteria (AC) — in a verifiable form
* Dependent issues

## 3. Test-Driven Development (TDD): Red-Green-Refactor

This project has predefined AC, required test names ([06] §4.1), and golden
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
* Record in the PR description that the RED-phase tests “failed correctly”
  (AI implementers must paste the “failure message during Red” into the PR body —
  if the test passes from the beginning, that is a sign that the test itself is wrong)
* Use the fixed AC test names from [06] §4.1. Additional tests are unrestricted
* Do not change the meaning of tests during refactoring (weakening assertions is prohibited)
* For bug fixes, **write a reproduction test first** (RED) → fix (GREEN)

## 4. PR Rules

* 1 PR = 1 issue. Keep diffs small (guideline: 500 lines or less for
  implementation + tests. If larger, consider splitting the issue)
* Required items in the PR template:
  - `Closes #NN`
  - Corresponding requirement IDs ([01] F/N/C/P/X)
  - AC verification results (test execution logs)
  - RED-phase failure confirmation (§3)
  - Judgment on whether an ADR is needed (whether [10] §6 applies)
* CI (build + all tests + clang-format + clang-tidy) must be green
* Merge to main by squash merge (keep history grouped by issue)

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

The checklist in the issue body is the **definitive definition of scope** and,
as a rule, is not changed after issue filing. Progress is visualized using the
checklist in the PR body, and reviewers compare the PR check state with the
implementation artifacts. The issue is closed by `Closes #NN` when the PR is
merged (checkboxes on the issue side may remain unchecked when closed).

## 6. Release Flow

1. All required issues for the release boundary (the table at the beginning of
   [07]) are closed
2. release-please generates the CHANGELOG and version PR from Conventional Commits
3. Merge the version PR → tag → `wheels.yml` publishes to PyPI

(END OF DOCUMENT)
