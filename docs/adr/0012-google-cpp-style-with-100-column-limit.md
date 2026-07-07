# ADR-0012: Google C++ style with 100-column limit

## Status

Accepted — 2026-07-07

## Context

A consistent code style is essential for a project that will be implemented
partly by AI coding agents. The style should be well documented, easy to
enforce with tools, and compatible with the naming conventions already used in
the design documents.

## Decision

We will use the Google C++ Style Guide as the base, with the column limit set
to 100 characters. Formatting will be enforced with clang-format.

## Consequences

* Good: Google style is widely known and well supported by clang-format.
* Good: The design documents already use Google-style naming (PascalCase
  methods, trailing `_` members, snake_case variables).
* Good: A 100-column limit balances readability with the need to keep lines
  short enough for side-by-side review.

## Options Considered

* **LLVM style** — rejected because its naming conventions differ from the
  design documents.
* **Google style with 80 columns** — rejected because 80 columns is overly
  restrictive for modern displays and template-heavy C++ code.
* **Google style with 100 columns** — selected for compatibility and
  practicality.

## References

* Issue #1
