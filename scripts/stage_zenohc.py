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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--platform", choices=("windows",), default="windows")
    args = parser.parse_args()
    if args.platform != "windows":
        raise ValueError("only the official Windows standalone archive is supported")

    url = (
        "https://github.com/eclipse-zenoh/zenoh-c/releases/download/"
        f"{VERSION}/zenoh-c-{VERSION}-x86_64-pc-windows-msvc-standalone.zip"
    )
    archive = urllib.request.urlopen(url, timeout=120).read()
    digest = hashlib.sha256(archive).hexdigest()
    if digest != WINDOWS_SHA256:
        raise RuntimeError(f"unexpected zenoh-c archive hash: {digest}")

    root = args.root.resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as source:
        for member in source.infolist():
            destination = (root / member.filename).resolve()
            if root not in destination.parents and destination != root:
                raise RuntimeError(f"unsafe archive path: {member.filename}")
            source.extract(member, root)
    license_path = root / "LICENSE"
    if not license_path.is_file():
        license_url = f"https://raw.githubusercontent.com/eclipse-zenoh/zenoh-c/{VERSION}/LICENSE"
        license_data = urllib.request.urlopen(license_url, timeout=60).read()
        if hashlib.sha256(license_data).hexdigest() != LICENSE_SHA256:
            raise RuntimeError("unexpected zenoh-c license hash")
        license_path.write_bytes(license_data)
    notice_path = root / "NOTICE.md"
    if not notice_path.is_file():
        notice_url = f"https://raw.githubusercontent.com/eclipse-zenoh/zenoh-c/{VERSION}/NOTICE.md"
        notice_data = urllib.request.urlopen(notice_url, timeout=60).read()
        if hashlib.sha256(notice_data).hexdigest() != NOTICE_SHA256:
            raise RuntimeError("unexpected zenoh-c notice hash")
        notice_path.write_bytes(notice_data)
    for required in (
        root / "include" / "zenoh.h",
        root / "lib" / "zenohc.dll.lib",
        root / "bin" / "zenohc.dll",
        license_path,
        notice_path,
    ):
        if not required.is_file():
            raise RuntimeError(f"staged zenoh-c tree is incomplete: {required}")
    print(f"staged zenoh-c {VERSION} at {root}")


if __name__ == "__main__":
    main()
