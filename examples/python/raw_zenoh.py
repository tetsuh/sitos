#!/usr/bin/env python3
"""Raw Zenoh client for the sitos payload-v1 scalar contract."""
from __future__ import annotations

import importlib.metadata
import re
import struct
import sys
import time
from typing import Any

_ENCODING = "zenoh/bytes;sitos.v1"
_PAYLOAD = bytes([2]) + struct.pack("<d", 240.0)
_DEADLINE = 8.0
_PREFIX_CHUNK = re.compile(r"^[A-Za-z0-9_.-]+$")


def _parse_prefix(argv: list[str]) -> str:
    if len(argv) != 2 or argv[0] != "--prefix":
        raise ValueError("exactly one --prefix <prefix> is required")
    prefix = argv[1]
    if not prefix or any(
        not _PREFIX_CHUNK.fullmatch(chunk) for chunk in prefix.split("/")
    ):
        raise ValueError(
            "--prefix must contain nonempty [A-Za-z0-9_.-]+ slash-separated chunks"
        )
    return prefix


def _reply_sample(reply: Any) -> tuple[str, bytes, str]:
    if reply.err is not None:
        raise RuntimeError(f"query error: {reply.err}")
    sample = reply.ok
    if sample is None:
        raise RuntimeError("query returned no sample")
    return str(sample.key_expr), sample.payload.to_bytes(), str(sample.encoding)


def run(prefix: str) -> int:
    try:
        if importlib.metadata.version("eclipse-zenoh") != "1.9.0":
            raise RuntimeError("eclipse-zenoh==1.9.0 is required")
        import zenoh

        key = f"{prefix}/base/examples/fov"
        verified = False
        with zenoh.open(zenoh.Config()) as session:
            deadline = time.monotonic() + _DEADLINE
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                session.put(key, _PAYLOAD, encoding=zenoh.Encoding(_ENCODING))
                replies = [
                    _reply_sample(reply)
                    for reply in session.get(key, timeout=min(0.5, remaining))
                ]
                if not replies:
                    continue
                if len(replies) != 1:
                    raise RuntimeError(f"unexpected reply count: {len(replies)}")
                reply_key, payload, encoding = replies[0]
                if reply_key != key:
                    raise RuntimeError(f"unexpected key: {reply_key}")
                if payload != _PAYLOAD:
                    raise RuntimeError("payload-v1 bytes differ from expected DP value")
                if encoding != _ENCODING:
                    raise RuntimeError(f"unexpected encoding: {encoding}")
                if payload[0] != 2 or struct.unpack("<d", payload[1:])[0] != 240.0:
                    raise RuntimeError("payload-v1 DP decode mismatch")
                verified = True
                break
        if not verified:
            raise TimeoutError("raw value was not queryable before the deadline")
        print("PYTHON_RAW_ZENOH_OK")
        return 0
    except BaseException as error:
        print(f"raw zenoh failed: {error}", file=sys.stderr)
        return 1


def main(argv: list[str] | None = None) -> int:
    try:
        prefix = _parse_prefix(sys.argv[1:] if argv is None else argv)
    except ValueError as error:
        print(f"raw zenoh argument error: {error}", file=sys.stderr)
        return 2
    return run(prefix)


if __name__ == "__main__":
    raise SystemExit(main())
