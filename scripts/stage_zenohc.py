#!/usr/bin/env python3
"""Stage the pinned official zenoh-c standalone archive."""

from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import urllib.request
import zipfile
from pathlib import Path

VERSION = "1.9.0"
WINDOWS_SHA256 = "ebaa3c1ed303cf42c5af666b750da761c16d158f698cf17e5e8e0b4bd5524442"
LICENSE_SHA256 = "01a44774f7b1a453595c7c6d7f7308284ba6a1059dc49e14dad6647e1d44a338"
NOTICE_SHA256 = "ce71024d9e85cd28e31b023859531c447c7ef16063a2ea2979d412c6680858ed"


def download_checked(url: str, expected_sha256: str, description: str) -> bytes:
    data = urllib.request.urlopen(url, timeout=120).read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected_sha256:
        raise RuntimeError(f"unexpected {description} hash: {digest}")
    return data


def extract_archive(root: Path, archive: bytes) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as source:
        for member in source.infolist():
            destination = (root / member.filename).resolve()
            if root not in destination.parents and destination != root:
                raise RuntimeError(f"unsafe archive path: {member.filename}")
            source.extract(member, root)


def ensure_upstream_file(root: Path, name: str, expected_sha256: str) -> Path:
    path = root / name
    if not path.is_file():
        url = f"https://raw.githubusercontent.com/eclipse-zenoh/zenoh-c/{VERSION}/{name}"
        path.write_bytes(download_checked(url, expected_sha256, f"zenoh-c {name}"))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected_sha256:
        raise RuntimeError(f"unexpected zenoh-c {name} hash: {digest}")
    return path


def verify_stage(root: Path, license_path: Path, notice_path: Path) -> None:
    required = (
        root / "include" / "zenoh.h",
        root / "lib" / "zenohc.dll.lib",
        root / "bin" / "zenohc.dll",
        license_path,
        notice_path,
    )
    for path in required:
        if not path.is_file():
            raise RuntimeError(f"staged zenoh-c tree is incomplete: {path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--platform", choices=("windows",), default="windows")
    args = parser.parse_args()
    if args.platform != "windows":
        raise ValueError("only the official Windows standalone archive is supported")

    archive_url = (
        "https://github.com/eclipse-zenoh/zenoh-c/releases/download/"
        f"{VERSION}/zenoh-c-{VERSION}-x86_64-pc-windows-msvc-standalone.zip"
    )
    root = args.root.resolve()
    extract_archive(root, download_checked(archive_url, WINDOWS_SHA256, "zenoh-c archive"))
    license_path = ensure_upstream_file(root, "LICENSE", LICENSE_SHA256)
    notice_path = ensure_upstream_file(root, "NOTICE.md", NOTICE_SHA256)
    verify_stage(root, license_path, notice_path)
    print(f"staged zenoh-c {VERSION} at {root}")


if __name__ == "__main__":
    main()
