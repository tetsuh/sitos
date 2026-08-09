# AGENTS.md — AI Implementer Entry Point

This file is the entry point for AI coding agents working on the **sitos**
project.

## Essential Pointers

- Development workflow: [CONTRIBUTING.md](CONTRIBUTING.md)
- Issue breakdown: [docs/07_issue_breakdown.md](docs/07_issue_breakdown.md)
- Requirements: [docs/01_requirements.md](docs/01_requirements.md)
- Build / test / packaging: [docs/06_build_test_packaging.md](docs/06_build_test_packaging.md)
- ADR process: [docs/10_adr_process.md](docs/10_adr_process.md)
- Dependency policy: [docs/09_dependency_policy.md](docs/09_dependency_policy.md)

## Absolute Rules (Summary)

1. **One issue, one branch, one PR**: branch name
   `feat/<n>-<short-kebab-description>`, PR body contains `Closes #<n>`.
2. **TDD**: write the AC tests first and confirm RED before implementation.
   Record the command, failing test, expected reason, and one representative failure line.
3. **Conventional Commits**: header line
   `<type>(<scope>): <summary> (#<issue>)`; body is `- ` bullet list only.
4. **English only** in code, comments, commit messages, issues, PRs, and docs.
5. **No internal keywords**: never commit `xcynthia`, `paramdb`, `demeter`, or
   similar internal project names to the public repository.
6. **Transport isolation**: raw zenoh-cpp API is allowed only under
   `src/transport/`.
7. **Wire / protocol changes require an ADR**: see
   [docs/10_adr_process.md](docs/10_adr_process.md) §6.
8. **Frozen scope**: copy the frozen Issue checklist into the PR. Material scope
   changes require an owner decision, renewed review, and an Issue split when needed.
9. **Owner merge authority**: never merge without an explicit owner instruction
   for the current PR head; authorization expires after a commit or blocking finding.
10. **Merge policy**: normal merge is the default; rebase merge and auto-merge
    are prohibited. The owner may choose squash merge.

## Starting a Task

1. Read the full target GitHub Issue and any applicable entry in
   [docs/07_issue_breakdown.md](docs/07_issue_breakdown.md).
2. Confirm that the owner has declared evolving design scope ready when required.
3. Read the referenced design sections and related ADRs from [docs/adr/](docs/adr/).
4. Follow the full workflow and TDD steps in `CONTRIBUTING.md`.
