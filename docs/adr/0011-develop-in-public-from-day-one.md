# ADR-0011: Develop in public from day one

## Status

Accepted — 2026-07-07

## Context

sitos is a public OSS project. Development history, issues, and design
discussions will be visible from the first commit. This requires operational
discipline to prevent internal identifiers, customer data, or proprietary
context from entering the public repository.

## Decision

We will develop sitos in public from day one. The repository will remain public,
and all commits, issues, and PRs will be written accordingly.

## Consequences

* Good: Transparency builds community trust.
* Good: External contributors can follow and participate from the start.
* Bad: Every change must be reviewed for accidental inclusion of internal
  keywords or confidential information.

## Options Considered

* **Private development then open-sourcing** — rejected because it would
  require a costly cleanup of commit history and issues.
* **Public from day one** — selected because it enforces clean operational
  discipline from the start.

## References

* Issue #1
