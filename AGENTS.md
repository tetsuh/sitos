# AGENTS.md — AI Implementer Entry Point

This file is the entry point for AI coding agents working on the **sitos**
project.

## Essential Pointers

- Development workflow: [docs/development_workflow.md](docs/development_workflow.md)
- Issue breakdown: [docs/07_issue_breakdown.md](docs/07_issue_breakdown.md)
- Requirements: [docs/01_requirements.md](docs/01_requirements.md)
- Build / test / packaging: [docs/06_build_test_packaging.md](docs/06_build_test_packaging.md)
- ADR process: [docs/10_adr_process.md](docs/10_adr_process.md)
- Dependency policy: [docs/09_dependency_policy.md](docs/09_dependency_policy.md)

## Absolute Rules (Summary)

1. **One issue, one branch, one PR**: branch name
   `feat/<n>-<short-kebab-description>`, PR body contains `Closes #<n>`.
2. **TDD**: write the AC tests first and confirm RED before implementation.
   Record the RED-phase failure output in the PR description.
3. **Conventional Commits**: header line
   `<type>(<scope>): <summary> (#<issue>)`; body is `- ` bullet list only.
4. **English only** in code, comments, commit messages, issues, PRs, and docs.
5. **No internal keywords**: never commit `xcynthia`, `paramdb`, `demeter`, or
   similar internal project names to the public repository.
6. **Transport isolation**: raw zenoh-cpp API is allowed only under
   `src/transport/`.
7. **Wire / protocol changes require an ADR**: see
   [docs/10_adr_process.md](docs/10_adr_process.md) §6.

## Starting a Task

1. Read the full target issue from
   [docs/07_issue_breakdown.md](docs/07_issue_breakdown.md).
2. Read the referenced design sections.
3. Read related ADRs from [docs/adr/](docs/adr/).
4. Follow the workflow and TDD steps above.
