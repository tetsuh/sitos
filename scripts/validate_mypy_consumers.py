#!/usr/bin/env python3
"""Validate installed-wheel public typing in an isolated working directory."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

EXPECTED_INVALID_DIAGNOSTICS = Counter(
    {
        (4, "call-arg"): 1,
        (5, "arg-type"): 1,
        (6, "arg-type"): 1,
        (7, "arg-type"): 1,
        (8, "arg-type"): 2,
    }
)


def run_mypy(python: Path, source: Path, cwd: Path) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.pop("PYTHONPATH", None)
    environment.pop("MYPYPATH", None)
    return subprocess.run(
        [
            str(python),
            "-m",
            "mypy",
            "--strict",
            "--no-incremental",
            "--show-error-codes",
            str(source),
        ],
        cwd=cwd,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def parse_diagnostics(output: str) -> Counter[tuple[int, str]]:
    matches = re.findall(
        r"typing_consumer_invalid\.py:(\d+): error: .*\[([a-z0-9-]+)\]$",
        output,
        flags=re.MULTILINE,
    )
    return Counter((int(line), code) for line, code in matches)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--tests-dir", type=Path, required=True)
    args = parser.parse_args()
    tests_dir = args.tests_dir.resolve()

    with tempfile.TemporaryDirectory() as directory:
        cwd = Path(directory)
        python = args.python.absolute()
        valid = run_mypy(python, tests_dir / "typing_consumer_valid.py", cwd)
        print(valid.stdout, end="")
        print(valid.stderr, end="")
        if valid.returncode != 0:
            raise RuntimeError("valid installed-wheel typing consumer failed")

        invalid = run_mypy(python, tests_dir / "typing_consumer_invalid.py", cwd)
        print(invalid.stdout, end="")
        print(invalid.stderr, end="")
        if invalid.returncode == 0:
            raise RuntimeError("invalid installed-wheel typing consumer unexpectedly passed")
        diagnostics = parse_diagnostics(invalid.stdout + invalid.stderr)
        if diagnostics != EXPECTED_INVALID_DIAGNOSTICS:
            raise RuntimeError(
                f"invalid typing diagnostics differ: expected {EXPECTED_INVALID_DIAGNOSTICS}, "
                f"received {diagnostics}"
            )


if __name__ == "__main__":
    main()
