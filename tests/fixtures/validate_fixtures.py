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
# §5 (batch), §5.1 (batch fixture), §6 (AckAttachmentV1 / AckResultV1, ADR-0028).

import struct
import sys
from pathlib import Path

FIXTURE_DIR = Path(__file__).resolve().parent / "payload_v1"
ACK_FIXTURE_DIR = Path(__file__).resolve().parent / "ack_v1"

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


def ack_attachment(uuid_hex: str) -> bytes:
    """ADR-0028 AckAttachmentV1: schema_version=1 + 16 RFC 4122 UUID bytes."""
    return bytes([1]) + bytes.fromhex(uuid_hex.replace("-", ""))


def ack_result(kind: int, status: int, durability: int, applied: int, failed_index: int,
               through_sequence: int, failed_sequence: int, message: str = "") -> bytes:
    """ADR-0028 AckResultV1: 32-byte little-endian header + UTF-8 message."""
    body = message.encode("utf-8")
    return struct.pack("<BBBBIIQQI", 1, kind, status, durability, applied, failed_index,
                       through_sequence, failed_sequence, len(body)) + body


NO_INDEX = 0xFFFFFFFF
NO_SEQUENCE = 0xFFFFFFFFFFFFFFFF
K_PUT, K_BATCH, K_FENCE = 1, 2, 3
D_APPLIED, D_SYNCED = 1, 2
S_OK, S_INVALID_ARGUMENT, S_ERROR, S_OUTCOME_UNKNOWN = 0, 7, 8, 9


def expected_ack() -> dict:
    return {
        "attachment_put_token": ack_attachment("550e8400-e29b-41d4-a716-446655440000"),
        "result_put_ok": ack_result(K_PUT, S_OK, D_APPLIED, 1, NO_INDEX, 0, NO_SEQUENCE),
        "result_put_outcome_unknown": ack_result(
            K_PUT, S_OUTCOME_UNKNOWN, D_APPLIED, 0, 0, 0, NO_SEQUENCE, "engine: 失敗"),
        "result_batch_ok": ack_result(K_BATCH, S_OK, D_APPLIED, 3, NO_INDEX, 0, NO_SEQUENCE),
        "result_batch_envelope_invalid": ack_result(
            K_BATCH, S_INVALID_ARGUMENT, D_APPLIED, 0, NO_INDEX, 0, NO_SEQUENCE),
        "result_batch_entry_invalid": ack_result(
            K_BATCH, S_INVALID_ARGUMENT, D_APPLIED, 0, 1, 0, NO_SEQUENCE),
        "result_batch_prefix_unknown": ack_result(
            K_BATCH, S_OUTCOME_UNKNOWN, D_APPLIED, 2, 2, 0, NO_SEQUENCE),
        "result_fence_synced_ok": ack_result(K_FENCE, S_OK, D_SYNCED, 0, NO_INDEX, 42, NO_SEQUENCE),
        "result_fence_failed_sequence": ack_result(
            K_FENCE, S_ERROR, D_APPLIED, 0, NO_INDEX, 5, 3, "lane 3 failed"),
    }


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

    acks = expected_ack()
    for name, expected in acks.items():
        actual = parse_hex_file(ACK_FIXTURE_DIR / f"{name}.hex")
        if actual != expected:
            failures += 1
            print(f"FAIL {name}: expected {expected.hex(' ')} got {actual.hex(' ')}")
        else:
            print(f"OK   {name}")

    status = "PASS" if failures == 0 else "FAIL"
    print(f"{status} (fixture validator): {len(singles)} singles + 1 batch + {len(acks)} ack, "
          f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())