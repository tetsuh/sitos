#!/usr/bin/env python3
"""Offline contract tests for the repository's public Markdown documentation."""

from __future__ import annotations

import re
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
ALLOWED_EXTERNAL_HOSTS = {"github.com", "zenoh.io", "adr.github.io"}
CI_BADGE = "![CI](https://github.com/tetsuh/sitos/actions/workflows/ci.yml/badge.svg)"
POINTER_BYTES = (
    b"# Development workflow moved\n\n"
    b"The canonical development workflow is "
    b"[CONTRIBUTING.md](../CONTRIBUTING.md).\n"
)
WORKFLOW_HEADINGS = (
    "## 1. Branching Strategy: trunk-based",
    "## 2. Ticket-Driven Development (TiDD)",
    "## 3. Test-Driven Development (TDD): Red-Green-Refactor",
    "## 4. PR Rules",
    "## 5. Instruction Template for AI Implementers",
    "## 6. Release Flow",
    "## 7. Milestone Design Review (horizontal pass)",
)
README_COMMANDS = (
    "cmake --preset dev-linux",
    "cmake --build --preset dev-linux",
    "ctest --preset dev-linux",
    "cmake --preset dev-windows",
    "cmake --build --preset dev-windows",
    "ctest --preset dev-windows",
)
README_TARGETS = {
    "examples/cpp/quickstart.cpp",
    "examples/python/quickstart.py",
    "docs/02_architecture.md",
}


class DocumentationContractError(AssertionError):
    """A Markdown document violates the frozen documentation contract."""


@dataclass(frozen=True)
class MarkdownToken:
    source: Path
    line: int
    destination: str
    image: bool
    raw: str


@dataclass(frozen=True)
class _OrdinarySegment:
    text: str
    first_line: int


@dataclass(frozen=True)
class _AdrRecord:
    number: str
    filename: str
    title: str
    status: str


@dataclass(frozen=True)
class _AdrIndexEntry:
    number: str
    filename: str
    title: str
    status: str


_FENCE_OPEN = re.compile(r"^( {0,3})(`{3,}|~{3,})(.*)$")
_REFERENCE_DEFINITION = re.compile(r"^ {0,3}\[[^\n]*\]:")
_MULTILINE_LINK = re.compile(
    r"(?:!?)\[[^\r\n]*\][ \t\v\f]*(?:\r\n|\r|\n)[ \t\v\f]*\("
)
_AUTOLINK = re.compile(r"<[A-Za-z][A-Za-z0-9+.-]*:[^<>\n]*>")
_RAW_LINK_HTML = re.compile(r"<(?i:a|img)(?=[\t\n\v\f\r />])")
_LINK_TOKEN = re.compile(r"(!?)\[([^\[\]\\\n]*)\]\(([^()\\\t\n\v\f\r ]+)\)")
_BRACKET_RUN = re.compile(r"`+")
_ADR_FILENAME = re.compile(r"^(?P<number>[0-9]{4})-[a-z0-9.-]+\.md$")
_ADR_HEADING = re.compile(r"^# ADR-(?P<number>[0-9]{4}): (?P<title>.+)$")
_ADR_INDEX_ROW_CANDIDATE = re.compile(r"^ {0,3}\| \[")
_ADR_INDEX_ROW = re.compile(
    r"^\| \[(?P<number>[0-9]{4})\]\((?P<filename>[^)]+)\) "
    r"\| (?P<title>[^|]+) \| (?P<status>[^|]+) \|$"
)
_ADR_SECTION_STATUS = re.compile(r"(?ms)^## Status\s*\n\s*(?P<status>[^\n]+)")
_ADR_LEGACY_STATUS = re.compile(r"(?m)^- Status:\s*(?P<status>[^\n]+)")
_ADR_SIMPLE_STATUS = re.compile(
    r"^(?P<status>Proposed|Accepted|Rejected|Deprecated)(?: — [0-9]{4}-[0-9]{2}-[0-9]{2})?$"
)
_ADR_SUPERSEDED_STATUS = re.compile(
    r"^(?P<status>Superseded by ADR-[0-9]{4})(?: — [0-9]{4}-[0-9]{2}-[0-9]{2})?$"
)


def _physical_lines(text: str) -> list[str]:
    lines: list[str] = []
    start = 0
    index = 0
    while index < len(text):
        if text[index] in "\r\n":
            end = index + 1
            if text[index] == "\r" and end < len(text) and text[end] == "\n":
                end += 1
            lines.append(text[start:end])
            start = end
            index = end
        else:
            index += 1
    if start < len(text):
        lines.append(text[start:])
    return lines


def _ordinary_segments(text: str, source: Path) -> list[_OrdinarySegment]:
    lines = _physical_lines(text)
    segments: list[_OrdinarySegment] = []
    ordinary: list[str] = []
    ordinary_first = 1
    fence_marker: str | None = None
    fence_length = 0
    fence_line = 0

    def flush() -> None:
        nonlocal ordinary
        if ordinary:
            segments.append(_OrdinarySegment("".join(ordinary), ordinary_first))
            ordinary = []

    for line_number, physical_line in enumerate(lines, start=1):
        line = physical_line.rstrip("\r\n")
        if fence_marker is not None:
            closing = re.fullmatch(
                rf" {{0,3}}{re.escape(fence_marker)}{{{fence_length},}}[\t ]*", line
            )
            if closing:
                fence_marker = None
                fence_length = 0
                fence_line = 0
            continue

        opening = _FENCE_OPEN.fullmatch(line)
        if opening:
            marker_run = opening.group(2)
            info = opening.group(3)
            if marker_run[0] == "`" and "`" in info:
                raise DocumentationContractError(
                    f"{source}:{line_number}: backtick fence info contains a backtick"
                )
            flush()
            fence_marker = marker_run[0]
            fence_length = len(marker_run)
            fence_line = line_number
            continue

        if physical_line.startswith("\t") or physical_line.startswith("    "):
            flush()
            continue

        if not ordinary:
            ordinary_first = line_number
        ordinary.append(physical_line)

    flush()
    if fence_marker is not None:
        raise DocumentationContractError(
            f"{source}:{fence_line}: unclosed {fence_marker * fence_length} fence"
        )
    return segments


def _mask_inline_code(segment: _OrdinarySegment, source: Path) -> str:
    text = segment.text
    masked = list(text)
    position = 0
    while True:
        opener = _BRACKET_RUN.search(text, position)
        if opener is None:
            break
        length = opener.end() - opener.start()
        search_from = opener.end()
        closer = None
        while True:
            candidate = _BRACKET_RUN.search(text, search_from)
            if candidate is None:
                line = segment.first_line + text.count("\n", 0, opener.start())
                raise DocumentationContractError(
                    f"{source}:{line}: unmatched inline-code delimiter of length {length}"
                )
            if candidate.end() - candidate.start() == length:
                closer = candidate
                break
            search_from = candidate.end()
        for index in range(opener.start(), closer.end()):
            if masked[index] not in "\r\n":
                masked[index] = " "
        position = closer.end()
    return "".join(masked)


def _has_prior_unclosed_bracket(line: str, token_start: int) -> bool:
    balance = 0
    for character in line[:token_start]:
        if character == "[":
            balance += 1
        elif character == "]" and balance:
            balance -= 1
    return balance != 0


def parse_markdown(source: Path, text: str) -> list[MarkdownToken]:
    """Return accepted inline links/images and reject every unsupported form."""
    tokens: list[MarkdownToken] = []
    for segment in _ordinary_segments(text, source):
        masked = _mask_inline_code(segment, source)
        if _MULTILINE_LINK.search(masked):
            raise DocumentationContractError(f"{source}: multiline links are unsupported")
        if _RAW_LINK_HTML.search(masked):
            raise DocumentationContractError(f"{source}: raw HTML links/images are unsupported")

        for offset, physical_line in enumerate(_physical_lines(masked), start=0):
            line = physical_line.rstrip("\r\n")
            line_number = segment.first_line + offset
            if _REFERENCE_DEFINITION.search(line):
                raise DocumentationContractError(
                    f"{source}:{line_number}: reference definitions are unsupported"
                )
            if "][" in line:
                raise DocumentationContractError(
                    f"{source}:{line_number}: reference links are unsupported"
                )
            if _AUTOLINK.search(line):
                raise DocumentationContractError(
                    f"{source}:{line_number}: autolinks are unsupported"
                )
            if re.search(r"\[!\[", line):
                raise DocumentationContractError(
                    f"{source}:{line_number}: linked images are unsupported"
                )

            matches = list(_LINK_TOKEN.finditer(line))
            closure_positions = {match.start(3) - 2 for match in matches}
            actual_closures = {match.start() for match in re.finditer(r"\]\(", line)}
            if closure_positions != actual_closures:
                raise DocumentationContractError(
                    f"{source}:{line_number}: malformed or unsupported inline link"
                )

            for match in matches:
                if _has_prior_unclosed_bracket(line, match.start()):
                    raise DocumentationContractError(
                        f"{source}:{line_number}: nested link labels are unsupported"
                    )
                tokens.append(
                    MarkdownToken(
                        source=source,
                        line=line_number,
                        destination=match.group(3),
                        image=bool(match.group(1)),
                        raw=match.group(0),
                    )
                )
    return tokens


def _strict_root(root: Path) -> Path:
    try:
        return root.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise DocumentationContractError(f"repository root cannot be resolved: {error}") from error


def resolve_local_target(token: MarkdownToken, root: Path) -> Path | None:
    destination = token.destination
    path_part = destination.split("#", 1)[0]
    if not path_part:
        return None
    try:
        target = (token.source.parent / path_part).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise DocumentationContractError(
            f"{token.source}:{token.line}: missing local target {destination!r}: {error}"
        ) from error
    strict_root = _strict_root(root)
    if not target.is_relative_to(strict_root):
        raise DocumentationContractError(
            f"{token.source}:{token.line}: local target escapes the repository: {destination!r}"
        )
    if not (target.is_file() or target.is_dir()):
        raise DocumentationContractError(
            f"{token.source}:{token.line}: local target is not a file or directory"
        )
    return target


def validate_destination(token: MarkdownToken, root: Path) -> Path | None:
    destination = token.destination
    if "%" in destination:
        raise DocumentationContractError(
            f"{token.source}:{token.line}: percent encoding is unsupported"
        )
    if "?" in destination:
        raise DocumentationContractError(
            f"{token.source}:{token.line}: query markers are unsupported"
        )
    if any(ord(character) < 32 or ord(character) == 127 for character in destination):
        raise DocumentationContractError(
            f"{token.source}:{token.line}: ASCII control characters are unsupported"
        )
    try:
        parsed = urlsplit(destination)
        _ = parsed.port
    except ValueError as error:
        raise DocumentationContractError(
            f"{token.source}:{token.line}: malformed URL {destination!r}: {error}"
        ) from error

    if parsed.scheme or parsed.netloc:
        if not destination.startswith("https://") or parsed.scheme != "https":
            raise DocumentationContractError(
                f"{token.source}:{token.line}: only lowercase HTTPS URLs are allowed"
            )
        if parsed.username is not None or parsed.password is not None or parsed.port is not None:
            raise DocumentationContractError(
                f"{token.source}:{token.line}: URL credentials and ports are forbidden"
            )
        if parsed.netloc.endswith(".") or parsed.netloc not in ALLOWED_EXTERNAL_HOSTS:
            raise DocumentationContractError(
                f"{token.source}:{token.line}: external host is not allowed: {parsed.netloc!r}"
            )
        return None

    if destination.startswith("/"):
        raise DocumentationContractError(
            f"{token.source}:{token.line}: absolute filesystem paths are forbidden"
        )
    return resolve_local_target(token, root)


def _normalize_adr_status(raw: str, source: Path) -> str:
    status = raw.strip()
    for pattern in (_ADR_SIMPLE_STATUS, _ADR_SUPERSEDED_STATUS):
        match = pattern.fullmatch(status)
        if match is not None:
            return match.group("status")
    raise DocumentationContractError(f"{source}: unsupported ADR status {status!r}")


def _adr_records(root: Path) -> list[_AdrRecord]:
    records: list[_AdrRecord] = []
    for path in sorted((root / "docs/adr").glob("[0-9][0-9][0-9][0-9]-*.md")):
        filename_match = _ADR_FILENAME.fullmatch(path.name)
        if filename_match is None:
            raise DocumentationContractError(f"{path}: malformed ADR filename")
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        heading_match = _ADR_HEADING.fullmatch(lines[0] if lines else "")
        if heading_match is None:
            raise DocumentationContractError(f"{path}: malformed ADR H1")
        if heading_match.group("number") != filename_match.group("number"):
            raise DocumentationContractError(f"{path}: ADR filename and H1 number differ")
        status_match = _ADR_SECTION_STATUS.search(text) or _ADR_LEGACY_STATUS.search(text)
        if status_match is None:
            raise DocumentationContractError(f"{path}: missing ADR status")
        records.append(
            _AdrRecord(
                number=filename_match.group("number"),
                filename=path.name,
                title=heading_match.group("title"),
                status=_normalize_adr_status(status_match.group("status"), path),
            )
        )
    if not records:
        raise DocumentationContractError("docs/adr: no numbered ADR files found")
    return records


def _adr_index_entries(root: Path) -> list[_AdrIndexEntry]:
    path = root / "docs/adr/README.md"
    lines = path.read_text(encoding="utf-8").splitlines()
    if "| ADR | Title | Status |" not in lines:
        raise DocumentationContractError(f"{path}: missing ADR/Title/Status index header")
    entries: list[_AdrIndexEntry] = []
    for line_number, line in enumerate(lines, start=1):
        if _ADR_INDEX_ROW_CANDIDATE.match(line) is None:
            continue
        match = _ADR_INDEX_ROW.fullmatch(line)
        if match is None:
            raise DocumentationContractError(f"{path}:{line_number}: malformed ADR index row")
        entries.append(
            _AdrIndexEntry(
                number=match.group("number"),
                filename=match.group("filename"),
                title=match.group("title").strip(),
                status=match.group("status").strip(),
            )
        )
    if not entries:
        raise DocumentationContractError(f"{path}: ADR index is empty")
    return entries


def validate_adr_index(root: Path) -> None:
    """Require a complete, ordered ADR index whose metadata matches each ADR."""
    records = _adr_records(root)
    entries = _adr_index_entries(root)
    expected_numbers = [record.number for record in records]
    actual_numbers = [entry.number for entry in entries]
    if actual_numbers != sorted(actual_numbers):
        raise DocumentationContractError("docs/adr/README.md: ADR rows are not numerically ordered")
    if len(actual_numbers) != len(set(actual_numbers)):
        raise DocumentationContractError("docs/adr/README.md: duplicate ADR number")
    filenames = [entry.filename for entry in entries]
    if len(filenames) != len(set(filenames)):
        raise DocumentationContractError("docs/adr/README.md: duplicate ADR target")
    if actual_numbers != expected_numbers:
        raise DocumentationContractError(
            "docs/adr/README.md: ADR numbers do not match the repository inventory"
        )

    records_by_filename = {record.filename: record for record in records}
    if set(filenames) != set(records_by_filename):
        raise DocumentationContractError(
            "docs/adr/README.md: ADR targets do not match the repository inventory"
        )
    for entry in entries:
        record = records_by_filename[entry.filename]
        if entry.number != record.number:
            raise DocumentationContractError(
                f"docs/adr/README.md: index number for {entry.filename} does not match"
            )
        if entry.title != record.title:
            raise DocumentationContractError(
                f"docs/adr/README.md: index title for ADR-{record.number} does not match"
            )
        if entry.status != record.status:
            raise DocumentationContractError(
                f"docs/adr/README.md: index status for ADR-{record.number} does not match"
            )


def markdown_corpus(root: Path) -> list[Path]:
    paths = {root / "README.md", root / "CONTRIBUTING.md", root / "AGENTS.md"}
    paths.update((root / ".github").glob("**/*.md"))
    paths.update((root / "docs").glob("**/*.md"))
    return sorted(paths)


def scan_corpus(root: Path) -> dict[Path, list[MarkdownToken]]:
    scanned: dict[Path, list[MarkdownToken]] = {}
    strict_root = _strict_root(root)
    for path in markdown_corpus(root):
        if not path.exists():
            raise DocumentationContractError(f"required public document is missing: {path}")
        if path.is_symlink() or not path.is_file():
            raise DocumentationContractError(
                f"public document must be a regular non-symlink file: {path}"
            )
        try:
            text = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError) as error:
            raise DocumentationContractError(f"cannot read {path} as UTF-8: {error}") from error
        tokens = parse_markdown(path, text.replace("\r\n", "\n").replace("\r", "\n"))
        for token in tokens:
            validate_destination(token, strict_root)
        scanned[path] = tokens
    return scanned


class MarkdownGrammarTest(unittest.TestCase):
    def parse(self, text: str) -> list[MarkdownToken]:
        return parse_markdown(Path("fixture.md"), text)

    def assert_rejected(self, text: str) -> None:
        with self.assertRaises(DocumentationContractError):
            self.parse(text)

    def test_accepts_supported_forms_and_ignores_code(self) -> None:
        text = """[link](target.md) ![badge](https://github.com/a/b) [F04]
[](empty-label.md) ![](empty-alt.png)
`[inline](ignored.md)`
``inside `[multiline](ignored.md)`
continues here``
```cpp
[x](fenced.md)
````
    [x](indented.md)
~~~python
[x](tilde.md)
~~~~
"""
        tokens = self.parse(text)
        self.assertEqual(
            [token.destination for token in tokens],
            ["target.md", "https://github.com/a/b", "empty-label.md", "empty-alt.png"],
        )
        self.assertEqual([token.image for token in tokens], [False, True, False, True])

    def test_rejects_unsupported_link_forms(self) -> None:
        cases = {
            "full reference": "[label][id]\n",
            "collapsed reference": "[label][]\n",
            "reference definition": "[id]: target.md\n",
            "multiline link": "[label]\n(target.md)\n",
            "CRLF multiline link": "[label]\r\n(target.md)\r\n",
            "CR multiline link": "[label]\r(target.md)\r",
            "vertical-tab LF multiline link": "[label]\v\n(target.md)\n",
            "vertical-tab CRLF multiline link": "[label]\v\r\n(target.md)\r\n",
            "vertical-tab CR multiline link": "[label]\v\r(target.md)\r",
            "form-feed LF multiline link": "[label]\f\n(target.md)\n",
            "form-feed CRLF multiline link": "[label]\f\r\n(target.md)\r\n",
            "form-feed CR multiline link": "[label]\f\r(target.md)\r",
            "multiline escaped label": "[bad\\\\label]\n(target.md)\n",
            "multiline escaped alt": "![bad\\\\alt]\n(target.png)\n",
            "form-feed physical line prefix": "\f    [x](bad destination.md)\n",
            "vertical-tab physical line prefix": "\v    [x](bad destination.md)\n",
            "multiline nested label": "[outer[inner]]\n(target.md)\n",
            "multiline nested alt": "![outer[alt]]\n(target.png)\n",
            "autolink": "<https://github.com/tetsuh/sitos>\n",
            "raw anchor": '<a href="target.md">x</a>\n',
            "vertical-tab raw anchor": '<a\vhref="target.md">x</a>\n',
            "multiline raw anchor": '<a\n href="target.md">x</a>\n',
            "raw image": '<img src="target.png">\n',
            "vertical-tab raw image": '<img\vsrc="target.png">\n',
            "linked image": "[![alt](badge.svg)](target.md)\n",
            "nested label": "[outer[inner](target.md)\n",
            "nested closing label": "[outer]inner](target.md)\n",
            "escaped label": "[bad\\label](target.md)\n",
            "multiline label": "[bad\nlabel](target.md)\n",
            "nested alt": "![bad[alt](target.png)\n",
            "nested closing alt": "![bad]alt](target.png)\n",
            "escaped alt": "![bad\\alt](target.png)\n",
            "multiline alt": "![bad\nalt](target.png)\n",
            "empty destination": "[label]()\n",
            "destination whitespace": "[label](target file.md)\n",
            "destination parentheses": "[label](target(file).md)\n",
            "destination backslash": "[label](target\\file.md)\n",
        }
        for name, text in cases.items():
            with self.subTest(name=name):
                self.assert_rejected(text)

    def test_rejects_unclosed_or_mismatched_code_delimiters(self) -> None:
        for name, text in {
            "unclosed fence": "```text\ncontent\n",
            "short fence closer": "````text\ncontent\n```\n",
            "wrong fence marker": "```text\ncontent\n~~~\n",
            "backtick in info": "```bad`info\n```\n",
            "unclosed inline": "`code\n",
            "mismatched inline": "``code`\n",
            "segment boundary": "`code\n    block\n`\n",
        }.items():
            with self.subTest(name=name):
                self.assert_rejected(text)

    def test_destination_policy_boundaries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            container = Path(temporary).resolve()
            root = container / "repo"
            root.mkdir()
            source = root / "doc.md"
            target = root / "target.md"
            outside = container / "outside.md"
            source.write_text("fixture\n", encoding="utf-8")
            target.write_text("target\n", encoding="utf-8")
            outside.write_text("outside\n", encoding="utf-8")
            symlink_created = False
            try:
                (root / "outside-link.md").symlink_to(outside)
                symlink_created = True
            except (NotImplementedError, OSError):
                # Windows hosts may not grant symlink creation to the test process.
                pass

            def token(destination: str) -> MarkdownToken:
                return MarkdownToken(source, 1, destination, False, f"[x]({destination})")

            self.assertEqual(validate_destination(token("target.md#section"), root), target)
            self.assertIsNone(validate_destination(token("#section"), root))
            self.assertIsNone(validate_destination(token("https://github.com/tetsuh/sitos"), root))

            rejected = {
                "percent": "target%2emd",
                "control": "target\x7f.md",
                "query": "target.md?raw=1",
                "nul": "target\x00.md",
                "absolute": "/tmp/target.md",
                "file URL": "file:///tmp/target.md",
                "escape": "../outside.md",
                "missing": "missing.md",
                "other scheme": "http://github.com/tetsuh/sitos",
                "authority non-HTTPS": "//github.com/tetsuh/sitos",
                "uppercase scheme": "HTTPS://github.com/tetsuh/sitos",
                "username": "https://user@github.com/tetsuh/sitos",
                "password": "https://user:pass@github.com/tetsuh/sitos",
                "port": "https://github.com:443/tetsuh/sitos",
                "trailing dot": "https://github.com./tetsuh/sitos",
                "disallowed host": "https://example.com/docs",
                "subdomain": "https://docs.github.com/tetsuh/sitos",
                "malformed port": "https://github.com:notaport/tetsuh/sitos",
            }
            if symlink_created:
                rejected["symlink escape"] = "outside-link.md"
            for name, destination in rejected.items():
                with self.subTest(name=name):
                    with self.assertRaises(DocumentationContractError):
                        validate_destination(token(destination), root)


class PublicDocumentationTest(unittest.TestCase):
    def test_adr_index_contract(self) -> None:
        validate_adr_index(ROOT)
        adr_index = (ROOT / "docs/adr/README.md").read_text(encoding="utf-8")
        overview = (ROOT / "docs/00_overview.md").read_text(encoding="utf-8")
        process = (ROOT / "docs/10_adr_process.md").read_text(encoding="utf-8")
        self.assertIn("sole comprehensive maintained ADR index", adr_index)
        self.assertIn("historical D1 through D13 summary", overview)
        self.assertIn("not a comprehensive ADR index", overview)
        self.assertIn("Every ADR PR must update", process)
        self.assertIn("docs/adr/README.md", process)

    def test_adr_index_contract_rejects_metadata_drift(self) -> None:
        valid_index = """# Architecture Decision Records (ADRs)

| ADR | Title | Status |
|---|---|---|
| [0001](0001-first-decision.md) | First decision | Accepted |
| [0002](0002-second-decision.md) | Second decision | Superseded by ADR-0001 |
"""
        mutations = {
            "missing": valid_index.replace(
                "| [0002](0002-second-decision.md) | Second decision | "
                "Superseded by ADR-0001 |\n",
                "",
            ),
            "extra": valid_index
            + "| [0003](0003-extra-decision.md) | Extra decision | Proposed |\n",
            "duplicate": valid_index.replace(
                "| [0002](0002-second-decision.md)",
                "| [0001](0001-first-decision.md)",
            ),
            "indented duplicate": valid_index.replace(
                "| [0002](0002-second-decision.md) | Second decision | "
                "Superseded by ADR-0001 |\n",
                "| [0002](0002-second-decision.md) | Second decision | "
                "Superseded by ADR-0001 |\n"
                " | [0001](0001-first-decision.md) | First decision | Accepted |\n",
            ),
            "misordered": valid_index.replace(
                "| [0001](0001-first-decision.md) | First decision | Accepted |\n"
                "| [0002](0002-second-decision.md) | Second decision | "
                "Superseded by ADR-0001 |\n",
                "| [0002](0002-second-decision.md) | Second decision | "
                "Superseded by ADR-0001 |\n"
                "| [0001](0001-first-decision.md) | First decision | Accepted |\n",
            ),
            "misnumbered": valid_index.replace("[0002]", "[0003]"),
            "mistitled": valid_index.replace("Second decision", "Stale title"),
            "stale status": valid_index.replace(
                "Superseded by ADR-0001", "Accepted"
            ),
        }
        for name, index in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                adr_directory = root / "docs/adr"
                adr_directory.mkdir(parents=True)
                (adr_directory / "README.md").write_text(index, encoding="utf-8")
                (adr_directory / "0001-first-decision.md").write_text(
                    "# ADR-0001: First decision\n\n## Status\n\n"
                    "Accepted — 2026-01-01\n",
                    encoding="utf-8",
                )
                (adr_directory / "0002-second-decision.md").write_text(
                    "# ADR-0002: Second decision\n\n- Status: Superseded by ADR-0001\n",
                    encoding="utf-8",
                )
                with self.assertRaises(DocumentationContractError):
                    validate_adr_index(root)

    def test_public_markdown_links(self) -> None:
        scanned = scan_corpus(ROOT)
        legacy = (ROOT / "docs/development_workflow.md").resolve(strict=True)
        for source, tokens in scanned.items():
            if source.resolve() == legacy:
                continue
            for token in tokens:
                parsed = urlsplit(token.destination)
                if parsed.scheme or parsed.netloc:
                    continue
                target = resolve_local_target(token, ROOT)
                self.assertNotEqual(
                    target,
                    legacy,
                    f"{source}:{token.line} must link to root CONTRIBUTING.md",
                )

    def test_readme_contract(self) -> None:
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        lines = readme.splitlines()
        for command in README_COMMANDS:
            self.assertIn(command, lines)
        tokens = parse_markdown(ROOT / "README.md", readme)
        destinations = {token.destination for token in tokens}
        self.assertTrue(README_TARGETS <= destinations)
        for component in ("StorageNode", "ParamStore", "ParamCache"):
            self.assertIn(component, readme)
        images = [token.raw for token in tokens if token.image]
        self.assertEqual(images, [CI_BADGE])

    def test_contributing_contract(self) -> None:
        path = ROOT / "CONTRIBUTING.md"
        self.assertTrue(path.is_file(), "root CONTRIBUTING.md must exist")
        contributing = path.read_text(encoding="utf-8")
        headings = [line for line in contributing.splitlines() if line.startswith("## ")]
        for heading in WORKFLOW_HEADINGS:
            self.assertIn(heading, headings)

    def test_legacy_workflow_pointer_is_exact(self) -> None:
        self.assertEqual((ROOT / "docs/development_workflow.md").read_bytes(), POINTER_BYTES)


if __name__ == "__main__":
    unittest.main()
