#!/usr/bin/env python3
"""Validate the contents and native dependencies of a repaired sitos wheel."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from email.parser import Parser
from pathlib import Path

from packaging.requirements import Requirement


FORBIDDEN_TOKENS = (
    "rocksdb",
    "gtest",
    "gmock",
    "cmake",
    "include/",
    "/include/",
    "benchmark",
    "build/",
    "/build/",
)
FORBIDDEN_SUFFIXES = (".a", ".lib", ".h", ".hh", ".hpp", ".cmake")
FORBIDDEN_BASENAMES = (
    "sitos_python_param_cache_fixture",
    "sitos_python_param_cache_fixture.exe",
    "sitos_python_param_store_fixture",
    "sitos_python_param_store_fixture.exe",
)


def validate_wheel_members(names: list[str]) -> None:
    for name in names:
        lowered = name.lower()
        base = name.rsplit("/", maxsplit=1)[-1].lower()
        if (
            any(token in lowered for token in FORBIDDEN_TOKENS)
            or lowered.endswith(FORBIDDEN_SUFFIXES)
            or base in FORBIDDEN_BASENAMES
        ):
            raise RuntimeError(f"forbidden wheel entry: {name}")


def validate_private_test_support_absent(names: list[str]) -> None:
    forbidden = ("_gil_test", "gil_boundary", "python_test_support")
    leaked = [name for name in names if any(token in name.lower() for token in forbidden)]
    if leaked:
        raise RuntimeError(f"private Python test support leaked into wheel: {', '.join(leaked)}")


def validate_public_typing_members(names: list[str]) -> None:
    required = {
        "sitos/__init__.pyi",
        "sitos/_sitos.pyi",
        "sitos/cache.pyi",
        "sitos/store.pyi",
        "sitos/node.pyi",
        "sitos/py.typed",
    }
    missing = sorted(required.difference(names))
    if missing:
        raise RuntimeError(f"wheel is missing public typing files: {', '.join(missing)}")


def validate_python_runtime_metadata(metadata_text: str) -> None:
    requirements = [
        Requirement(value)
        for value in Parser().parsestr(metadata_text).get_all("Requires-Dist", [])
    ]
    numpy_requirements = [
        requirement for requirement in requirements if requirement.name.lower() == "numpy"
    ]
    if (
        len(numpy_requirements) != 1
        or numpy_requirements[0].marker is not None
        or ">=2.0" not in {str(specifier) for specifier in numpy_requirements[0].specifier}
    ):
        raise RuntimeError("wheel metadata must require NumPy 2 with numpy>=2.0")
    if any(requirement.name.lower() == "mypy" for requirement in requirements):
        raise RuntimeError("wheel metadata must not declare mypy as a runtime dependency")


def cmake_version() -> str:
    cmake = Path(__file__).parents[1] / "CMakeLists.txt"
    match = re.search(r"project\(sitos VERSION ([0-9]+\.[0-9]+\.[0-9]+)", cmake.read_text())
    if match is None:
        raise RuntimeError("could not determine the CMake project version")
    return match.group(1)


def is_runtime_member(name: str) -> bool:
    base = name.rsplit("/", maxsplit=1)[-1].lower()
    return (base.startswith("libzenohc") and ".so" in base) or (
        base.startswith("zenohc") and base.endswith(".dll")
    )


def runtime_members(names: list[str]) -> list[str]:
    return [name for name in names if is_runtime_member(name)]


def require_member(names: list[str], suffix: str, message: str) -> str:
    matches = [name for name in names if name.lower().endswith(suffix.lower())]
    if len(matches) != 1:
        raise RuntimeError(message)
    return matches[0]


def validate_linux_native_dependencies(extension: Path, runtime: Path) -> None:
    readelf = shutil.which("readelf")
    ldd = shutil.which("ldd")
    if readelf is None or ldd is None:
        raise RuntimeError("readelf and ldd are required for Linux wheel validation")
    dynamic = subprocess.run(
        [readelf, "-d", str(extension)], check=True, capture_output=True, text=True
    ).stdout
    if "libzenohc" not in dynamic:
        raise RuntimeError("sitos._sitos does not declare a zenoh-c native dependency")
    environment = {"PATH": os.environ["PATH"], "LD_LIBRARY_PATH": str(runtime.parent)}
    resolved = subprocess.run(
        [ldd, str(extension)], check=True, capture_output=True, text=True, env=environment
    ).stdout
    runtime_line = next((line for line in resolved.splitlines() if "libzenohc" in line), None)
    if runtime_line is None or "=>" not in runtime_line:
        raise RuntimeError("sitos._sitos does not resolve zenoh-c from the wheel runtime directory")
    resolved_name = runtime_line.partition("=>")[2].strip().split(maxsplit=1)[0]
    if resolved_name == "not" or Path(resolved_name).resolve() != runtime.resolve():
        raise RuntimeError("sitos._sitos does not resolve zenoh-c from the wheel runtime directory")


def validate_windows_native_dependencies(extension: Path, runtime: Path) -> None:
    dumpbin = shutil.which("dumpbin")
    if dumpbin is None:
        raise RuntimeError("dumpbin is required for Windows wheel validation")
    output = subprocess.run(
        [dumpbin, "/DEPENDENTS", str(extension)], check=True, capture_output=True, text=True
    ).stdout
    if not re.search(r"zenohc(?:-[0-9a-f]+)?\.dll", output, re.I):
        raise RuntimeError("sitos._sitos does not declare a zenoh-c native dependency")
    if not runtime.is_file():
        raise RuntimeError("the wheel zenoh-c runtime is missing")


def validate_native_dependencies(root: Path, extension_name: str, runtime_name: str, platform: str) -> None:
    extension = root / extension_name
    runtime = root / runtime_name
    if platform == "linux":
        validate_linux_native_dependencies(extension, runtime)
    elif platform == "windows":
        validate_windows_native_dependencies(extension, runtime)
    else:
        raise RuntimeError(f"unsupported platform: {platform}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel", type=Path)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--version")
    args = parser.parse_args()
    version = args.version or cmake_version()

    with zipfile.ZipFile(args.wheel) as wheel:
        names = wheel.namelist()
        validate_wheel_members(names)
        validate_private_test_support_absent(names)
        validate_public_typing_members(names)
        extension_suffix = ".pyd" if args.platform == "windows" else ".so"
        extensions = [
            name for name in names if name.startswith("sitos/_sitos") and name.lower().endswith(extension_suffix)
        ]
        if len(extensions) != 1:
            raise RuntimeError("wheel does not contain exactly one sitos._sitos extension")
        extension = extensions[0]
        runtimes = runtime_members(names)
        if len(runtimes) != 1:
            raise RuntimeError("wheel must contain exactly one zenoh-c runtime")
        lowered_names = [name.lower() for name in names]
        if not any(
            ".dist-info/licenses/" in name and name.endswith("license-zenoh-c")
            for name in lowered_names
        ):
            raise RuntimeError("wheel does not contain the zenoh-c license metadata")
        if not any(
            ".dist-info/licenses/" in name and name.endswith("notice-zenoh-c.md")
            for name in lowered_names
        ):
            raise RuntimeError("wheel does not contain the zenoh-c notice metadata")
        metadata = require_member(names, ".dist-info/metadata", "wheel does not contain .dist-info/METADATA")
        metadata_text = wheel.read(metadata).decode("utf-8")
        if f"Version: {version}" not in metadata_text:
            raise RuntimeError("wheel metadata version does not match CMake version")
        validate_python_runtime_metadata(metadata_text)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wheel.extractall(root)
            validate_native_dependencies(root, extension, runtimes[0], args.platform)

    print(f"validated wheel {args.wheel.name}")


if __name__ == "__main__":
    main()
