#!/usr/bin/env python3
"""Behavioral tests for the internal-keyword guard (Issue #180).

The guard script reads its word list from the KEYWORD_GUARD_WORDS repository secret in CI. These
tests run the same script against synthetic terms in temporary Git repositories, so no real term
is needed. Every test also checks that the configured term never appears in the output.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "keyword_guard.sh"
WORKFLOW = ROOT / ".github" / "workflows" / "keyword-guard.yml"


def _workflow_step(step_name: str) -> tuple[str, str]:
    """Return a workflow step's YAML header and de-indented ``run`` body."""
    lines = WORKFLOW.read_text(encoding="utf-8").splitlines()
    name_index = lines.index(f"      - name: {step_name}")
    run_index = next(
        index for index in range(name_index + 1, len(lines)) if lines[index] == "        run: |"
    )
    body = []
    for line in lines[run_index + 1 :]:
        if line and not line.startswith("          "):
            break
        body.append(line[10:] if line else "")
    return "\n".join(lines[name_index:run_index]), "\n".join(body) + "\n"


def _make_repo(base: Path, tracked: dict[str, str]) -> Path:
    repo = base / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    for relative_path, contents in tracked.items():
        path = repo / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
    subprocess.run(["git", "-C", str(repo), "add", "--all"], check=True)
    return repo


def _run_guard(
    repo: Path, words: str | None, extra_path: Path | None = None, timeout: float = 20
) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C.UTF-8"
    environment.pop("KEYWORD_GUARD_WORDS", None)
    if words is not None:
        environment["KEYWORD_GUARD_WORDS"] = words
    if extra_path is not None:
        environment["PATH"] = f"{extra_path}{os.pathsep}{environment['PATH']}"
    return subprocess.run(
        ["bash", "--noprofile", "--norc", str(SCRIPT)],
        cwd=repo,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )


class KeywordGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.base = Path(self._temporary.name)
        self._runs = 0

    def scan(
        self, words: str | None, tracked: dict[str, str], timeout: float = 20
    ) -> subprocess.CompletedProcess[str]:
        self._runs += 1
        run_base = self.base / f"run-{self._runs}"
        run_base.mkdir()
        return _run_guard(_make_repo(run_base, tracked), words, timeout=timeout)

    def assert_no_terms(self, output: str, words: list[str]) -> None:
        for word in words:
            self.assertNotIn(word.casefold(), output.casefold())

    # Configuration of the secret.

    def test_missing_word_list_fails_with_actionable_message(self) -> None:
        completed = self.scan(None, {"clean.txt": "safe\n"})
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("KEYWORD_GUARD_WORDS repository secret is not set", completed.stdout)
        self.assertIn("configure it before merging", completed.stdout)

    def test_word_list_with_only_separators_and_whitespace_fails(self) -> None:
        completed = self.scan(" , \t,\n ", {"clean.txt": "safe\n"})
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("word list is empty", completed.stdout)
        self.assertIn("set the repository secret", completed.stdout)

    def test_word_list_with_only_unicode_whitespace_fails(self) -> None:
        for whitespace in (" ", " ", "　"):
            with self.subTest(whitespace=hex(ord(whitespace))):
                completed = self.scan(f",{whitespace},", {"clean.txt": "safe\n"})
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("word list is empty", completed.stdout)

    # Clean trees pass.

    def test_clean_tree_passes(self) -> None:
        completed = self.scan("Ac.me", {"clean.txt": "safe\n"})
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("keyword guard: clean", completed.stdout)

    def test_clean_tree_with_absent_word_terminates(self) -> None:
        completed = self.scan("not-present", {"clean.txt": "safe\n"}, timeout=5)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(completed.stdout.splitlines(), ["keyword guard: clean"])

    # Synthetic positives fail without disclosing the term.

    def test_content_hit_fails_without_disclosing_the_term(self) -> None:
        word = "Ac.me"
        completed = self.scan(word, {"content.txt": f"before {word} after\n"})
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("prohibited word in contents (1 occurrence(s))", output)
        self.assert_no_terms(output, [word])

    def test_case_insensitive_word_boundary_matching(self) -> None:
        cases = [
            ("needle", {"a.txt": "A NEEDLE here\n"}, 1),
            ("needle", {"a.txt": "needles and haystack\n"}, 0),
        ]
        for word, tracked, expected in cases:
            with self.subTest(contents=next(iter(tracked.values()))):
                completed = self.scan(word, tracked)
                self.assertEqual(completed.returncode, expected)
                self.assert_no_terms(completed.stdout + completed.stderr, [word])

    def test_path_hit_is_masked_literally_for_regex_metacharacters(self) -> None:
        word = "Ac.Me"
        completed = self.scan(word, {"notes/ac.me-record.txt": "safe\n"})
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("prohibited word in path notes/ -record.txt (1 occurrence(s))", output)
        self.assert_no_terms(output, [word])

    def test_whitespace_inside_a_term_is_removed(self) -> None:
        # Terms are single words: "Ac me" is the term "Acme", as CONTRIBUTING.md section 4.2 states.
        joined = self.scan("Ac me", {"a.txt": "acme\n"})
        self.assertNotEqual(joined.returncode, 0)
        self.assert_no_terms(joined.stdout + joined.stderr, ["acme"])
        spaced = self.scan("Ac me", {"a.txt": "ac me\n"})
        self.assertEqual(spaced.returncode, 0)

    def test_line_breaks_in_the_word_list_do_not_drop_later_terms(self) -> None:
        # ADV-206-001: a secret stored with line breaks must keep every term after the first line.
        cases = [
            ("alpha,\nbeta", {"a.txt": "beta\n"}),
            ("alpha,\r\nbeta", {"a.txt": "beta\n"}),
            ("\nbeta", {"a.txt": "beta\n"}),
            ("al\npha,beta", {"a.txt": "alpha\n"}),
            ("alpha\nbeta", {"a.txt": "alphabeta\n"}),
        ]
        for words, tracked in cases:
            with self.subTest(words=repr(words)):
                completed = self.scan(words, tracked)
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0, output)
                self.assert_no_terms(output, ["alpha", "beta"])

    def test_later_terms_of_a_multiline_list_are_masked_in_reports(self) -> None:
        completed = self.scan("alpha,\nbeta", {"beta.txt": "alpha\n"})
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assert_no_terms(output, ["alpha", "beta"])

    def test_every_listed_term_is_checked(self) -> None:
        completed = self.scan("alpha, beta", {"a.txt": "beta\n"})
        self.assertNotEqual(completed.returncode, 0)
        self.assert_no_terms(completed.stdout + completed.stderr, ["alpha", "beta"])

    def test_skeleton_substrings_and_replacement_markers_never_leak(self) -> None:
        for word in ["err", "rror", "fi", "ile", ":", "::", "*", "**", "***"]:
            cases = [
                ("clean", {"safe.txt": "safe\n"}, 0),
                ("contents", {"safe.txt": f"before {word} after\n"}, 1),
                ("path", {f"{word}.txt": "safe\n"}, 1),
            ]
            for mode, tracked, expected in cases:
                with self.subTest(word=word, mode=mode):
                    completed = self.scan(word, tracked)
                    output = completed.stdout + completed.stderr
                    self.assertEqual(completed.returncode, expected)
                    self.assert_no_terms(output, [word])
                    self.assertIn("keyword guard" if mode == "clean" else "prohibited word", output)
                    # These terms occur inside the workflow command syntax itself, so a hit is
                    # reported as one plain masked line rather than as a command.
                    if word in {"err", "rror", "fi", "ile", ":", "::"}:
                        self.assertNotIn("::", output)
                        self.assertEqual(len(output.splitlines()), 1)

    def test_configured_terms_cannot_cross_rendered_annotation_boundaries(self) -> None:
        cases = [
            ("::keyword", {"clean.txt": "safe\n"}, 0),
            ("=safe", {"safe.txt": "=safe\n"}, 1),
            ("txt::prohibited", {"safe.txt": "txt::prohibited\n"}, 1),
            ("::prohibited", {"::prohibited.txt": "safe\n"}, 1),
        ]
        for word, tracked, expected in cases:
            with self.subTest(word=word):
                completed = self.scan(word, tracked)
                output = completed.stdout + completed.stderr
                self.assertEqual(completed.returncode, expected)
                self.assertEqual(len(output.splitlines()), 1)
                self.assertFalse(output.startswith("::"))
                self.assertNotIn("::error", output)
                self.assert_no_terms(output, [word])

    def test_unicode_case_equivalent_path_hits_are_masked(self) -> None:
        cases = [("σ", "docs/ο-ς.txt", "docs/ο- .txt"), ("s", "dir/ſ-afe.txt", "dir/ -afe.txt")]
        for word, path, masked_path in cases:
            with self.subTest(word=word):
                completed = self.scan(word, {path: "safe\n"})
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(f"prohibited word in path {masked_path}", output)
                self.assert_no_terms(output, [word])

    def test_configured_terms_are_masked_in_fixed_reporter_messages(self) -> None:
        clean = self.scan("clean", {"safe.txt": "safe\n"})
        self.assertEqual(clean.returncode, 0)
        self.assertEqual((clean.stdout + clean.stderr).splitlines(), ["keyword guard:  "])

        contents = self.scan("contents", {"safe.txt": "contents\n"})
        output = contents.stdout + contents.stderr
        self.assertEqual(contents.returncode, 1)
        self.assert_no_terms(output, ["contents"])
        self.assertIn("prohibited word in", output)
        self.assertIn("(1 occurrence(s))", output)

        occurrence = self.scan("occurrence", {"safe.txt": "occurrence\n"})
        output = occurrence.stdout + occurrence.stderr
        self.assertNotEqual(occurrence.returncode, 0)
        self.assertIn("(1  (s))", output)
        self.assert_no_terms(output, ["occurrence"])

    def test_repeated_matches_terminate_and_mask_every_occurrence(self) -> None:
        path_hit = self.scan("x", {"x-x.log": "safe\n"})
        output = path_hit.stdout + path_hit.stderr
        self.assertNotEqual(path_hit.returncode, 0)
        self.assertIn("prohibited word in path  - .log (2 occurrence(s))", output)
        self.assert_no_terms(output, ["x"])

        reporter_hit = self.scan("s", {"clean.txt": "s\n"})
        output = reporter_hit.stdout + reporter_hit.stderr
        self.assertNotEqual(reporter_hit.returncode, 0)
        self.assertIn("prohibited word", output)
        self.assert_no_terms(output, ["s"])

    def test_reserved_command_words_fall_back_to_masked_plain_text(self) -> None:
        for word in ("error", "file"):
            with self.subTest(word=word):
                completed = self.scan(word, {f"{word}.txt": "safe\n"})
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0)
                self.assertNotIn("::", output)
                self.assertIn("prohibited word in path", output)
                self.assertIn(" .txt", output)
                self.assert_no_terms(output, [word])

    # Workflow-command control bytes in paths stay inert.

    def test_command_control_bytes_in_tracked_paths_stay_inert(self) -> None:
        for control in ("::", "%0A", "%0D", ",", "=", "\r", "\n"):
            with self.subTest(control=repr(control)):
                completed = self.scan("needle", {f"docs/needle{control}report.txt": "safe\n"})
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(len(output.splitlines()), 1)
                self.assertNotIn("::", output)
                self.assertEqual(output.count("prohibited word in path"), 1)
                self.assert_no_terms(output, ["needle"])

    def test_command_control_bytes_in_annotated_contents_diagnostics_stay_inert(self) -> None:
        for control in ("::", "%0A", "%0D", ",", "=", "\r", "\n"):
            with self.subTest(control=repr(control)):
                completed = self.scan("needle", {f"docs/report{control}.txt": "safe needle\n"})
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(len(output.splitlines()), 1)
                self.assertNotIn("::", output)
                self.assertIn(": prohibited word in contents (1 occurrence(s))", output)
                self.assertNotIn("prohibited word in path", output)
                self.assert_no_terms(output, ["needle"])

    def test_configured_terms_spanning_command_control_paths_stay_inert(self) -> None:
        # Commas delimit the configured list, so "foo,bar" is two terms and two findings.
        cases = [("foo::bar", 1), ("foo%0Abar", 1), ("foo%0Dbar", 1), ("foo,bar", 2), ("foo=bar", 1)]
        for word, expected_reports in cases:
            with self.subTest(word=word):
                completed = self.scan(word, {f"docs/{word}.txt": "safe\n"})
                output = completed.stdout + completed.stderr
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(len(output.splitlines()), expected_reports)
                self.assertNotIn("::", output)
                self.assertEqual(output.count("prohibited word in path"), expected_reports)
                self.assert_no_terms(output, [word])

    # Scan failures are never reported as clean.

    def _scan_links(self, words: str, links: dict[str, str], untracked: dict[str, str]):
        self._runs += 1
        repo = self.base / f"links-{self._runs}"
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        for name, target in links.items():
            (repo / name).symlink_to(target)
        subprocess.run(["git", "-C", str(repo), "add", "--all"], check=True)
        for relative_path, contents in untracked.items():
            path = repo / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        return _run_guard(repo, words)

    def test_symlinks_are_scanned_by_their_tracked_target_text(self) -> None:
        # ADV-206-002: a tracked symlink's content is its target text; the guard never follows it.
        cases = [
            ("needle", {"link.txt": "needle-target"}, {}, 1),
            ("null", {"link.txt": "/dev/null"}, {}, 1),
            ("needle", {"link.txt": "needle.txt"}, {"needle.txt": "clean\n"}, 1),
            ("needle", {"link.txt": "needle-dir/target.txt"}, {"needle-dir/target.txt": "clean\n"}, 1),
            ("needle", {"link.txt": "target.txt"}, {"target.txt": "needle\n"}, 0),
            ("needle", {"link.txt": "missing-target"}, {}, 0),
        ]
        for words, links, untracked, expected in cases:
            with self.subTest(target=next(iter(links.values())), untracked=sorted(untracked)):
                completed = self._scan_links(words, links, untracked)
                output = completed.stdout + completed.stderr
                self.assertEqual(completed.returncode, expected, output)
                self.assert_no_terms(output, [words])
                if expected:
                    self.assertIn("prohibited word in contents", output)

    def test_unreadable_tracked_contents_are_not_clean(self) -> None:
        repo = _make_repo(self.base, {"clean.txt": "safe\n", "gone.txt": "safe\n"})
        (repo / "gone.txt").unlink()
        completed = _run_guard(repo, "needle")
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("unable to scan tracked contents", output)
        self.assertNotIn("keyword guard: clean", output)

    def test_git_ls_files_failure_is_not_clean(self) -> None:
        repo = _make_repo(self.base, {"clean.txt": "safe\n"})
        real_git = shutil.which("git")
        self.assertIsNotNone(real_git)
        fake_bin = self.base / "bin"
        fake_bin.mkdir()
        fake_git = fake_bin / "git"
        fake_git.write_text(
            "#!/bin/sh\n"
            'if [ "$1" = "ls-files" ]; then\n'
            '  printf "clean.txt\\0"\n'
            "  exit 23\n"
            "fi\n"
            f'exec "{real_git}" "$@"\n',
            encoding="utf-8",
        )
        fake_git.chmod(0o755)
        completed = _run_guard(repo, "needle", extra_path=fake_bin)
        output = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("keyword guard: unable to enumerate tracked files", output)
        self.assertNotIn("keyword guard: clean", output)

    # Workflow wiring.

    def test_fork_pull_requests_fail_closed(self) -> None:
        header, script = _workflow_step("Reject unscannable fork pull requests")
        self.assertIn("github.event.pull_request.head.repo.full_name != github.repository", header)
        completed = subprocess.run(
            ["bash", "--noprofile", "--norc", "-e"],
            cwd=self.base,
            input=script,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(
            completed.stdout.splitlines(),
            [
                "::error::Fork pull requests cannot be scanned by the keyword guard.",
                "Re-run the branch from a branch in this repository before merging.",
            ],
        )

    def test_workflow_reads_the_secret_and_never_uses_pull_request_target(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("KEYWORD_GUARD_WORDS: ${{ secrets.KEYWORD_GUARD_WORDS }}", workflow)
        self.assertIn("bash scripts/keyword_guard.sh", workflow)
        self.assertIn("python3 -B tests/ci/test_keyword_guard.py", workflow)
        triggers = workflow.split("\npermissions:", 1)[0]
        self.assertNotIn("pull_request_target", triggers)
        # Every push and every pull request (Issue #180): no branch or path filter.
        self.assertIn("\n  pull_request:\n", triggers)
        self.assertIn("\n  push:\n", triggers)
        for narrowing in ("branches", "paths", "tags"):
            self.assertNotIn(narrowing, triggers)
        self.assertIn("persist-credentials: false", workflow)


if __name__ == "__main__":
    unittest.main()
