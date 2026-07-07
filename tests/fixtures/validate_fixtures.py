#!/usr/bin/env python3
# Copyright 2026 sitos contributors
# SPDX-License-Identifier: Apache-2.0
#
# Independent golden-fixture validator.
#
# Regenerates every payload_v1 fixture from first principles using only the
# Python standard library (struct, UTF-8 encoding) and asserts byte-for-byte
# equality with the `.hex` files on disk. Shares no code with the C++ codec or
# the C++ tests, so a wrong fixture baked into both the implementation and the
# C++ test would still be caught here.
#
# Spec: docs/03_wire_protocol.md §2 (payload v1), §2.3 (golden fixtures),
# §5 (batch), §5.1 (batch fixture).

import struct
import sys
from pathlib import Path

FIXTURE_DIR = Path(__file__).resolve().parent / "payload_v1"

# Canonical quiet-NaN body bytes (LE), matching docs/03 §2.3 (dp_nan).
# Bit pattern 0x7ff8000000000000.
CANONICAL_NAN = bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F])

T_BOOL, T_S64, T_DP, T_STR, T_BYTES = 0, 1, 2, 3, 4


def parse_hex_file(path: Path) -> bytes:
    """Parse a `.hex` file as whitespace-separated hex pairs."""
    out = bytearray()
    for tok in path.read_text(encoding="utf-8").split():
        out.append(int(tok, 16))
    return bytes(out)


def single(tag: int, body: bytes) -> bytes:
    return bytes([tag]) + body


def expected_single() -> dict:
    return {
        "bool_false": single(T_BOOL, bytes([0x00])),
        "bool_true": single(T_BOOL, bytes([0x01])),
        "s64_zero": single(T_S64, struct.pack("<q", 0)),
        "s64_minus1": single(T_S64, struct.pack("<q", -1)),
        "s64_i32max": single(T_S64, struct.pack("<q", 2147483647)),
        "dp_zero": single(T_DP, struct.pack("<d", 0.0)),
        "dp_240": single(T_DP, struct.pack("<d", 240.0)),
        "dp_nan": single(T_DP, CANONICAL_NAN),
        "str_empty": single(T_STR, b""),
        "str_ascii": single(T_STR, "abc".encode("utf-8")),
        "str_utf8": single(T_STR, "穀".encode("utf-8")),
        "bytes_empty": single(T_BYTES, bytes()),
        "bytes_0102ff": single(T_BYTES, bytes([0x01, 0x02, 0xFF])),
    }


def expected_batch() -> bytes:
    """The §5.1 batch fixture, built independently with struct."""
    entries = [
        ("recon/fov", T_DP, struct.pack("<d", 240.0)),
        ("recon/kernel", T_STR, "sharp".encode("utf-8")),
    ]
    out = bytearray()
    out += struct.pack("<I", len(entries))
    for key, tag, body in entries:
        key_bytes = key.encode("utf-8")
        out += struct.pack("<I", len(key_bytes))
        out += key_bytes
        out += bytes([tag])
        out += struct.pack("<I", len(body))
        out += body
    return bytes(out)


def main() -> int:
    failures = 0
    singles = expected_single()

    for name, expected in singles.items():
        actual = parse_hex_file(FIXTURE_DIR / f"{name}.hex")
        if actual != expected:
            failures += 1
            print(f"FAIL {name}: expected {expected.hex(' ')} got {actual.hex(' ')}")
        else:
            print(f"OK   {name}")

    batch_actual = parse_hex_file(FIXTURE_DIR / "batch_base_two_entries.hex")
    batch_expected = expected_batch()
    if batch_actual != batch_expected:
        failures += 1
        print(f"FAIL batch_base_two_entries: expected {batch_expected.hex(' ')}\n"
              f"     got {batch_actual.hex(' ')}")
    else:
        print("OK   batch_base_two_entries")

    status = "PASS" if failures == 0 else "FAIL"
    print(f"{status} (fixture validator): {len(singles)} singles + 1 batch, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())