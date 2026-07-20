#!/usr/bin/env python3
"""Validate the contents and metadata of a repaired sitos wheel."""

from __future__ import annotations

import argparse
import re
import zipfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel", type=Path)
    parser.add_argument("version", nargs="?")
    args = parser.parse_args()
    version = args.version
    if version is None:
        cmake = Path(__file__).parents[1] / "CMakeLists.txt"
        match = re.search(r"project\(sitos VERSION ([0-9]+\.[0-9]+\.[0-9]+)", cmake.read_text())
        if match is None:
            raise RuntimeError("could not determine the CMake project version")
        version = match.group(1)

    with zipfile.ZipFile(args.wheel) as wheel:
        names = wheel.namelist()
        forbidden = ("rocksdb", "gtest", "gmock", "sitosTargets.cmake", "CMakeLists.txt")
        for name in names:
            lowered = name.lower()
            if any(token.lower() in lowered for token in forbidden):
                raise RuntimeError(f"forbidden wheel entry: {name}")
        if not any(name.startswith("sitos/_sitos") for name in names):
            raise RuntimeError("wheel does not contain sitos._sitos")
        if not any("libzenohc" in name.lower() or name.lower().endswith("/zenohc.dll") for name in names):
            raise RuntimeError("wheel does not contain the zenoh-c runtime")
        if not any(name.lower().endswith("sitos/licenses/license") for name in names):
            raise RuntimeError("wheel does not contain the zenoh-c license")
        metadata = next(name for name in names if name.endswith(".dist-info/METADATA"))
        metadata_text = wheel.read(metadata).decode("utf-8")
        if f"Version: {version}" not in metadata_text:
            raise RuntimeError("wheel metadata version does not match CMake version")

    print(f"validated wheel {args.wheel.name}")


if __name__ == "__main__":
    main()
