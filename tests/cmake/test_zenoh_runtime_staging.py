#!/usr/bin/env python3
"""Regression contract for race-free Windows zenoh-c runtime staging."""
from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def _run(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    helper = args.source_root.resolve() / "cmake" / "StageZenohRuntime.cmake"
    if not helper.is_file():
        raise AssertionError(f"runtime staging helper is absent: {helper}")

    with tempfile.TemporaryDirectory(prefix="sitos-zenoh-stage-") as raw_directory:
        root = Path(raw_directory)
        source = root / "source"
        build = root / "build"
        source.mkdir()
        (source / "zenohc.dll").write_bytes(b"sitos-zenoh-runtime\n")
        (source / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
        targets = "\n".join(
            f"add_executable(probe_{index} main.c)\n"
            f"sitos_copy_zenohc(probe_{index})"
            for index in range(32)
        )
        (source / "CMakeLists.txt").write_text(
            "\n".join(
                (
                    "cmake_minimum_required(VERSION 3.20)",
                    "project(stage_probe C)",
                    "set(WIN32 TRUE)",
                    'set(SITOS_ZENOHC_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/zenohc.dll")',
                    f'include("{helper.as_posix()}")',
                    targets,
                    "",
                )
            ),
            encoding="utf-8",
        )

        generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        _run(["cmake", "-S", str(source), "-B", str(build), *generator], cwd=root)
        completed = _run(
            ["cmake", "--build", str(build), "--parallel", "32"], cwd=root
        )
        output = completed.stdout + completed.stderr
        if output.count("Stage shared zenoh-c runtime") != 1:
            raise AssertionError(
                "expected one directory-scoped runtime stage operation, got:\n" + output
            )
        staged = build / "zenohc.dll"
        if staged.read_bytes() != (source / "zenohc.dll").read_bytes():
            raise AssertionError("staged zenoh-c runtime content mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
