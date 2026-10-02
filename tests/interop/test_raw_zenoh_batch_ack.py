"""Raw zenoh-python batch and acknowledgement interoperability (docs/03 §5 and §6)."""

from __future__ import annotations

import struct
import sys
import threading
import time
import uuid
from dataclasses import dataclass

import zenoh

import raw_zenoh_test_support as support

BOOL_TAG = 0
S64_TAG = 1
DP_TAG = 2
STR_TAG = 3
BYTES_TAG = 4
BATCH_ENCODING = "zenoh/bytes;sitos.v1.batch"
ACK_ENCODING = "zenoh/bytes;sitos.v1.ack"
ACK_ATTACHMENT_VERSION = 1
ACK_RESULT_HEADER_SIZE = 32
ACK_RESULT_MAX_MESSAGE = 1024
ACK_KIND_PUT = 1
ACK_KIND_BATCH = 2
ACK_KINDS = {ACK_KIND_PUT, ACK_KIND_BATCH, 3}
ACK_DURABILITY_APPLIED = 1
ACK_DURABILITIES = {ACK_DURABILITY_APPLIED, 2}
STATUS_OK = 0
STATUS_INVALID_KEY = 6
STATUS_INVALID_ARGUMENT = 7
# docs/03 §6 closed wire allowlist; Timeout = 3 is client-only.
WIRE_STATUSES = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11}
# docs/03 §6 and ADR-0028 classify these as definite validation rejections
# without fixing one Status per case, so the interop test accepts either.
DEFINITE_REJECTIONS = {STATUS_INVALID_KEY, STATUS_INVALID_ARGUMENT}
NO_FAILED_INDEX = 0xFFFFFFFF
NO_FAILED_SEQUENCE = 0xFFFFFFFFFFFFFFFF
SCENARIO_TIMEOUT_SECONDS = 8.0
ABSENCE_WINDOW_SECONDS = 1.0

# docs/03 §5.1 batch_base_two_entries, copied verbatim from the specification.
BATCH_BASE_TWO_ENTRIES_HEX = (
    "02 00 00 00 "
    "09 00 00 00 72 65 63 6f 6e 2f 66 6f 76 02 08 00 00 00 00 00 00 00 00 00 6e 40 "
    "0c 00 00 00 72 65 63 6f 6e 2f 6b 65 72 6e 65 6c 03 05 00 00 00 73 68 61 72 70"
)


@dataclass(frozen=True)
class BatchEntry:
    key: str
    tag: int
    body: bytes


@dataclass(frozen=True)
class AckResult:
    kind: int
    status: int
    durability: int
    applied_count: int
    failed_index: int
    through_sequence: int
    failed_sequence: int
    message: str


def _dp(key: str, value: float) -> BatchEntry:
    return BatchEntry(key, DP_TAG, struct.pack("<d", value))


def _str(key: str, value: str) -> BatchEntry:
    return BatchEntry(key, STR_TAG, value.encode("utf-8"))


def _encode_batch(entries: list[BatchEntry]) -> bytes:
    out = bytearray(struct.pack("<I", len(entries)))
    for entry in entries:
        key = entry.key.encode("utf-8")
        out += struct.pack("<I", len(key)) + key
        out += bytes([entry.tag]) + struct.pack("<I", len(entry.body)) + entry.body
    return bytes(out)


def _decode_batch(payload: bytes) -> list[BatchEntry]:
    offset = 0

    def take(size: int) -> bytes:
        nonlocal offset
        assert offset + size <= len(payload), "truncated sitos.v1.batch payload"
        chunk = payload[offset : offset + size]
        offset += size
        return chunk

    (count,) = struct.unpack("<I", take(4))
    entries: list[BatchEntry] = []
    for _ in range(count):
        (key_length,) = struct.unpack("<I", take(4))
        key = take(key_length).decode("utf-8")
        tag = take(1)[0]
        assert tag <= BYTES_TAG, f"unknown value tag {tag}"
        (value_length,) = struct.unpack("<I", take(4))
        entries.append(BatchEntry(key, tag, take(value_length)))
    assert offset == len(payload), "trailing bytes after sitos.v1.batch entries"
    return entries


def _ack_attachment(token: uuid.UUID) -> bytes:
    assert token.version == 4
    return bytes([ACK_ATTACHMENT_VERSION]) + token.bytes


def _decode_ack_result(payload: bytes) -> AckResult:
    assert len(payload) >= ACK_RESULT_HEADER_SIZE, "truncated AckResultV1"
    version, kind, status, durability = payload[:4]
    applied_count, failed_index = struct.unpack_from("<II", payload, 4)
    through_sequence, failed_sequence = struct.unpack_from("<QQ", payload, 12)
    (message_length,) = struct.unpack_from("<I", payload, 28)
    assert version == 1
    assert kind in ACK_KINDS
    assert status in WIRE_STATUSES
    assert durability in ACK_DURABILITIES
    assert message_length <= ACK_RESULT_MAX_MESSAGE
    assert len(payload) == ACK_RESULT_HEADER_SIZE + message_length
    message = payload[ACK_RESULT_HEADER_SIZE:].decode("utf-8")
    return AckResult(
        kind,
        status,
        durability,
        applied_count,
        failed_index,
        through_sequence,
        failed_sequence,
        message,
    )


def _assert_ack(
    result: AckResult,
    kind: int,
    statuses: set[int],
    applied_count: int,
    failed_index: int,
) -> None:
    """Check every Put/Batch field; the message is informative (ADR-0028)."""
    assert result.kind == kind
    assert result.status in statuses
    assert result.durability == ACK_DURABILITY_APPLIED
    assert result.applied_count == applied_count
    assert result.failed_index == failed_index
    assert result.through_sequence == 0
    assert result.failed_sequence == NO_FAILED_SEQUENCE


def _read_replies(session: zenoh.Session, key: str) -> list[support.WireSample]:
    replies: list[support.WireSample] = []
    for reply in session.get(key, timeout=0.5):
        assert reply.err is None, f"zenoh get failed: {reply.err}"
        sample = reply.ok
        assert sample is not None
        replies.append(support.copy_sample(sample))
    return replies


def _wait_for_reply(session: zenoh.Session, key: str) -> support.WireSample:
    deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        replies = _read_replies(session, key)
        if replies:
            assert len(replies) == 1, f"unexpected replies: {replies!r}"
            assert replies[0].key == key
            return replies[0]
    raise AssertionError(f"no reply before the deadline: {key}")


def _assert_no_reply(session: zenoh.Session, key: str) -> None:
    deadline = time.monotonic() + ABSENCE_WINDOW_SECONDS
    while time.monotonic() < deadline:
        assert _read_replies(session, key) == [], f"unexpected reply for {key}"


def _assert_values(session: zenoh.Session, prefix: str, entries: list[BatchEntry]) -> None:
    """Wait under one deadline until every key holds its last encoded value."""
    expected = {
        f"{prefix}/base/{entry.key}": bytes([entry.tag]) + entry.body for entry in entries
    }
    observed: dict[str, list[support.WireSample]] = {}
    deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        observed = {key: _read_replies(session, key) for key in expected}
        if all(
            len(replies) == 1 and replies[0].payload == expected[key]
            for key, replies in observed.items()
        ):
            break
    for key, payload in expected.items():
        assert len(observed.get(key, [])) == 1, f"no single reply for {key}: {observed!r}"
        support.assert_wire_sample(
            observed[key][0],
            expected_key=key,
            expected_payload=payload,
            expected_encoding=support.CANONICAL_SITOS_ENCODING,
        )


def _put_acknowledged(
    session: zenoh.Session, prefix: str, key: str, payload: bytes, encoding: str
) -> AckResult:
    token = uuid.uuid4()
    session.put(
        key,
        payload,
        encoding=zenoh.Encoding(encoding),
        attachment=_ack_attachment(token),
    )
    ack_key = f"{prefix}/meta/ack/{token}"
    reply = _wait_for_reply(session, ack_key)
    assert reply.encoding == ACK_ENCODING
    return _decode_ack_result(reply.payload)


def _establish_route(session: zenoh.Session, prefix: str) -> None:
    """Prove the raw client reaches StorageNode before single-submission checks."""
    key = f"{prefix}/base/raw/route"
    entry = _dp("raw/route", 1.0)
    payload = bytes([entry.tag]) + entry.body
    deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        session.put(
            key, payload, encoding=zenoh.Encoding(support.CANONICAL_SITOS_ENCODING)
        )
        if _read_replies(session, key):
            return
    raise AssertionError("raw client never reached StorageNode")


def _assert_raw_interop_imports() -> None:
    support.assert_no_sitos_import(sys.modules[__name__])
    support.assert_no_sitos_import(support)


def test_raw_zenoh_client_can_send_batch() -> None:
    _assert_raw_interop_imports()
    fixture_entries = [_dp("recon/fov", 240.0), _str("recon/kernel", "sharp")]
    assert _encode_batch(fixture_entries) == bytes.fromhex(BATCH_BASE_TWO_ENTRIES_HEX)
    assert _decode_batch(bytes.fromhex(BATCH_BASE_TWO_ENTRIES_HEX)) == fixture_entries

    entries = [
        BatchEntry("raw/batch/flag", BOOL_TAG, b"\x01"),
        BatchEntry("raw/batch/count", S64_TAG, struct.pack("<q", -42)),
        _dp("raw/batch/dup", 1.5),
        _str("raw/batch/text", "穀"),
        BatchEntry("raw/batch/blob", BYTES_TAG, b"\x01\x02\xff"),
        _dp("raw/batch/dup", -2.25),
    ]
    final = list({entry.key: entry for entry in entries}.values())
    assert [entry.body for entry in final if entry.key == "raw/batch/dup"] == [
        struct.pack("<d", -2.25)
    ]

    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            _establish_route(session, fixture.prefix)
            batch_key = f"{fixture.prefix}/base/:batch"
            session.put(
                batch_key, _encode_batch(entries), encoding=zenoh.Encoding(BATCH_ENCODING)
            )
            _assert_values(session, fixture.prefix, final)
            assert _read_replies(session, batch_key) == []


def test_raw_zenoh_client_decodes_cpp_batch() -> None:
    _assert_raw_interop_imports()
    entries = [
        _str("cpp/batch/mode", "fast"),
        _dp("cpp/batch/dup", 3.0),
        BatchEntry("cpp/batch/count", S64_TAG, struct.pack("<q", 7)),
        BatchEntry("cpp/batch/flag", BOOL_TAG, b"\x00"),
        BatchEntry("cpp/batch/blob", BYTES_TAG, b""),
        _dp("cpp/batch/dup", 4.0),
    ]

    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            fixture.subscribe_batch()
            observed: list[support.WireSample] = []
            observed_event = threading.Event()

            def receive(sample: zenoh.Sample) -> None:
                observed.append(support.copy_sample(sample))
                observed_event.set()

            batch_key = f"{fixture.prefix}/base/:batch"
            subscriber = session.declare_subscriber(batch_key, receive)
            cpp_observations: list[list[tuple[str, int, bytes]]] = []
            try:
                deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
                while not observed_event.is_set() and time.monotonic() < deadline:
                    cpp_observations.append(
                        fixture.put_batch(
                            [(entry.key, entry.tag, entry.body) for entry in entries]
                        )
                    )
                    observed_event.wait(min(0.5, max(0.0, deadline - time.monotonic())))
                assert observed_event.is_set(), "raw subscriber missed the C++ batch"
            finally:
                subscriber.undeclare()

            expected = [(entry.key, entry.tag, entry.body) for entry in entries]
            assert cpp_observations
            assert all(observation == expected for observation in cpp_observations)
            for sample in observed:
                assert sample.key == batch_key
                assert sample.encoding == BATCH_ENCODING
                decoded = _decode_batch(sample.payload)
                assert [(entry.key, entry.tag, entry.body) for entry in decoded] == (
                    cpp_observations[0]
                )
                assert _encode_batch(decoded) == sample.payload


def test_raw_zenoh_batch_and_put_ack_round_trip() -> None:
    _assert_raw_interop_imports()
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            _establish_route(session, fixture.prefix)

            put_entry = _dp("ack/put", 11.0)
            put_key = f"{fixture.prefix}/base/{put_entry.key}"
            put_result = _put_acknowledged(
                session,
                fixture.prefix,
                put_key,
                bytes([put_entry.tag]) + put_entry.body,
                support.CANONICAL_SITOS_ENCODING,
            )
            _assert_ack(put_result, ACK_KIND_PUT, {STATUS_OK}, 1, NO_FAILED_INDEX)
            _assert_values(session, fixture.prefix, [put_entry])

            batch_entries = [
                _dp("ack/batch/a", 1.0),
                _str("ack/batch/b", "two"),
                _dp("ack/batch/a", 3.0),
            ]
            batch_key = f"{fixture.prefix}/base/:batch"
            batch_result = _put_acknowledged(
                session, fixture.prefix, batch_key, _encode_batch(batch_entries), BATCH_ENCODING
            )
            _assert_ack(batch_result, ACK_KIND_BATCH, {STATUS_OK}, 3, NO_FAILED_INDEX)
            _assert_values(session, fixture.prefix, batch_entries[1:])

            envelope_result = _put_acknowledged(
                session, fixture.prefix, batch_key, b"\x01\x00\x00", BATCH_ENCODING
            )
            _assert_ack(
                envelope_result, ACK_KIND_BATCH, DEFINITE_REJECTIONS, 0, NO_FAILED_INDEX
            )

            rejected = [_dp("ack/rejected/first", 5.0), _dp("ack/rejected/bad key", 6.0)]
            entry_result = _put_acknowledged(
                session, fixture.prefix, batch_key, _encode_batch(rejected), BATCH_ENCODING
            )
            _assert_ack(entry_result, ACK_KIND_BATCH, DEFINITE_REJECTIONS, 0, 1)
            _assert_no_reply(session, f"{fixture.prefix}/base/ack/rejected/first")


def test_raw_zenoh_ackless_batch_creates_no_ack_entry() -> None:
    _assert_raw_interop_imports()
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            _establish_route(session, fixture.prefix)
            assert fixture.ack_entries() == 0

            entries = [_dp("ackless/value", 8.5), _str("ackless/label", "plain")]
            session.put(
                f"{fixture.prefix}/base/:batch",
                _encode_batch(entries),
                encoding=zenoh.Encoding(BATCH_ENCODING),
            )
            single = _dp("ackless/single", 9.5)
            session.put(
                f"{fixture.prefix}/base/{single.key}",
                bytes([single.tag]) + single.body,
                encoding=zenoh.Encoding(support.CANONICAL_SITOS_ENCODING),
            )
            _assert_values(session, fixture.prefix, [*entries, single])
            # StorageNode claims a token before mutation, so visible values with an
            # unchanged registry prove neither ack-less write created a meta/ack entry.
            assert fixture.ack_entries() == 0

            # Positive control: one acknowledged batch is visible to the same counter.
            acknowledged = _put_acknowledged(
                session,
                fixture.prefix,
                f"{fixture.prefix}/base/:batch",
                _encode_batch([_dp("ackless/control", 10.5)]),
                BATCH_ENCODING,
            )
            _assert_ack(acknowledged, ACK_KIND_BATCH, {STATUS_OK}, 1, NO_FAILED_INDEX)
            assert fixture.ack_entries() == 1


def test_raw_zenoh_malformed_ack_attachment_is_rejected() -> None:
    _assert_raw_interop_imports()
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            _establish_route(session, fixture.prefix)

            # 17-byte attachments that are not AckAttachmentV1 (docs/03 §6 step 2).
            unknown_version_token = uuid.uuid4()
            session.put(
                f"{fixture.prefix}/base/:batch",
                _encode_batch([_dp("malformed/version", 1.0)]),
                encoding=zenoh.Encoding(BATCH_ENCODING),
                attachment=bytes([2]) + unknown_version_token.bytes,
            )
            non_v4 = uuid.UUID("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
            assert non_v4.version == 1
            non_v4_entry = _dp("malformed/uuid", 2.0)
            session.put(
                f"{fixture.prefix}/base/{non_v4_entry.key}",
                bytes([non_v4_entry.tag]) + non_v4_entry.body,
                encoding=zenoh.Encoding(support.CANONICAL_SITOS_ENCODING),
                attachment=bytes([ACK_ATTACHMENT_VERSION]) + non_v4.bytes,
            )

            sentinel = _dp("malformed/sentinel", 3.0)
            sentinel_result = _put_acknowledged(
                session,
                fixture.prefix,
                f"{fixture.prefix}/base/:batch",
                _encode_batch([sentinel]),
                BATCH_ENCODING,
            )
            _assert_ack(sentinel_result, ACK_KIND_BATCH, {STATUS_OK}, 1, NO_FAILED_INDEX)
            _assert_values(session, fixture.prefix, [sentinel])
            assert fixture.ack_entries() == 1
            _assert_no_reply(session, f"{fixture.prefix}/meta/ack/{unknown_version_token}")
            for key in ("malformed/version", "malformed/uuid"):
                _assert_no_reply(session, f"{fixture.prefix}/base/{key}")


def test_raw_zenoh_batch_aliases_are_not_accepted() -> None:
    _assert_raw_interop_imports()
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            _establish_route(session, fixture.prefix)

            # `$batch` never reaches the wire: zenoh rejects the key expression.
            dollar_rejections = 0
            try:
                zenoh.KeyExpr(f"{fixture.prefix}/base/$batch")
            except zenoh.ZError:
                dollar_rejections += 1
            try:
                session.put(
                    f"{fixture.prefix}/base/$batch",
                    _encode_batch([_dp("alias/dollar", 1.0)]),
                    encoding=zenoh.Encoding(BATCH_ENCODING),
                )
            except zenoh.ZError:
                dollar_rejections += 1
            assert dollar_rejections == 2

            subscribed = zenoh.KeyExpr(f"{fixture.prefix}/**")
            at_key = f"{fixture.prefix}/base/@batch"
            assert not subscribed.intersects(zenoh.KeyExpr(at_key))
            at_token = uuid.uuid4()
            session.put(
                at_key,
                _encode_batch([_dp("alias/at", 2.0)]),
                encoding=zenoh.Encoding(BATCH_ENCODING),
                attachment=_ack_attachment(at_token),
            )

            tilde_result = _put_acknowledged(
                session,
                fixture.prefix,
                f"{fixture.prefix}/base/~batch",
                _encode_batch([_dp("alias/tilde", 3.0)]),
                BATCH_ENCODING,
            )
            # `~batch` reaches StorageNode as an ordinary, invalid single-value key.
            _assert_ack(tilde_result, ACK_KIND_PUT, DEFINITE_REJECTIONS, 0, 0)

            sentinel = _dp("alias/sentinel", 4.0)
            sentinel_result = _put_acknowledged(
                session,
                fixture.prefix,
                f"{fixture.prefix}/base/:batch",
                _encode_batch([sentinel]),
                BATCH_ENCODING,
            )
            _assert_ack(sentinel_result, ACK_KIND_BATCH, {STATUS_OK}, 1, NO_FAILED_INDEX)
            _assert_values(session, fixture.prefix, [sentinel])
            # Only the `~batch` rejection and the sentinel claimed tokens.
            assert fixture.ack_entries() == 2
            _assert_no_reply(session, f"{fixture.prefix}/meta/ack/{at_token}")
            for alias in ("at", "tilde"):
                _assert_no_reply(session, f"{fixture.prefix}/base/alias/{alias}")
            for alias in ("@batch", "~batch"):
                _assert_no_reply(session, f"{fixture.prefix}/base/{alias}")
