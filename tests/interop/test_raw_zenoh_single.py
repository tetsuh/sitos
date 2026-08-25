"""Wire-level C03 interoperability tests using zenoh-python and the wire specification."""

from __future__ import annotations

import ast
import importlib.metadata
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from unittest.mock import Mock, call

import pytest
import zenoh

import raw_zenoh_test_support as support

DP_TAG = 2
QUERY_TIMEOUT_SECONDS = 0.5
SCENARIO_TIMEOUT_SECONDS = 8.0


def _encode_dp(value: float) -> bytes:
    return bytes([DP_TAG]) + struct.pack("<d", value)


def _decode_dp(payload: bytes) -> float:
    assert len(payload) == 9
    assert payload[0] == DP_TAG
    return struct.unpack("<d", payload[1:])[0]


def _assert_no_sitos_import(module: ModuleType) -> None:
    path = Path(module.__file__ or "")
    assert path.is_file(), f"cannot inspect imported module {module.__name__}"
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    imported_roots: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported_roots.update(alias.name.partition(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module is not None:
            imported_roots.add(node.module.partition(".")[0])
    assert "sitos" not in imported_roots


def _read_exact_reply(session: zenoh.Session, key: str) -> list[support.WireSample]:
    replies: list[support.WireSample] = []
    for reply in session.get(key, timeout=QUERY_TIMEOUT_SECONDS):
        assert reply.err is None, f"zenoh get failed: {reply.err}"
        sample = reply.ok
        assert sample is not None
        replies.append(support.copy_sample(sample))
    return replies


def _query_until_matches(
    session: zenoh.Session,
    key: str,
    payload: bytes,
    before_query: Callable[[], None] | None = None,
) -> support.WireSample:
    deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if before_query is not None:
            before_query()
        replies = _read_exact_reply(session, key)
        if not replies:
            continue
        assert len(replies) == 1, f"unexpected replies: {replies!r}"
        sample = replies[0]
        support.assert_wire_sample(
            sample,
            expected_key=key,
            expected_payload=payload,
            expected_encoding=support.CANONICAL_SITOS_ENCODING,
        )
        return sample
    raise AssertionError(f"value was not queryable before the deadline: {key}")


def _put_until_query_matches(
    session: zenoh.Session, key: str, payload: bytes
) -> support.WireSample:
    def put() -> None:
        session.put(
            key,
            payload,
            encoding=zenoh.Encoding(support.CANONICAL_SITOS_ENCODING),
        )

    return _query_until_matches(session, key, payload, put)


def test_raw_zenoh_client_can_put_and_get() -> None:
    expected_version = os.environ.get(
        "SITOS_EXPECTED_ZENOH_PYTHON_VERSION", "1.9.0"
    )
    assert importlib.metadata.version("eclipse-zenoh") == expected_version
    _assert_no_sitos_import(sys.modules[__name__])
    _assert_no_sitos_import(support)

    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            raw_key = f"{fixture.prefix}/base/raw/roundtrip"
            raw_payload = _encode_dp(240.0)
            raw_reply = _put_until_query_matches(session, raw_key, raw_payload)
            assert _decode_dp(raw_reply.payload) == 240.0

            cpp_key = f"{fixture.prefix}/base/cpp/subscribed"
            cpp_payload = _encode_dp(125.5)
            observed: list[support.WireSample] = []
            observed_event = threading.Event()

            def receive(sample: zenoh.Sample) -> None:
                observed.append(support.copy_sample(sample))
                observed_event.set()

            subscriber = session.declare_subscriber(f"{fixture.prefix}/base/**", receive)
            try:
                deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
                while not observed_event.is_set() and time.monotonic() < deadline:
                    fixture.put_dp("cpp/subscribed", 125.5)
                    observed_event.wait(min(0.5, max(0.0, deadline - time.monotonic())))
                assert observed_event.is_set(), (
                    "raw subscriber did not observe the C++ ParamStore put"
                )
            finally:
                subscriber.undeclare()

            assert observed
            for sample in observed:
                support.assert_wire_sample(
                    sample,
                    expected_key=cpp_key,
                    expected_payload=cpp_payload,
                    expected_encoding=support.CANONICAL_SITOS_ENCODING,
                )

            fixture.put_dp("snapshot/source", -17.25)
            fixture.create_session()
            snapshot_key = f"{fixture.prefix}/snap/{fixture.session_id}/snapshot/source"
            snapshot_reply = _query_until_matches(
                session, snapshot_key, _encode_dp(-17.25)
            )
            assert _decode_dp(snapshot_reply.payload) == -17.25


def test_raw_zenoh_fence_payload_and_control_isolation() -> None:
    _assert_no_sitos_import(sys.modules[__name__])
    _assert_no_sitos_import(support)

    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            publisher = uuid.UUID("8b8f3a62-7dd5-4c40-8a2b-28f71331fe41")
            receiver = uuid.UUID("123e4567-e89b-42d3-a456-426614174000")
            lane = bytes([1]) + publisher.bytes + struct.pack("<Q", 1)
            key = f"{fixture.prefix}/base/raw/fence-lane"
            payload = _encode_dp(19.5)
            session.put(
                key,
                payload,
                encoding=zenoh.Encoding(support.CANONICAL_SITOS_ENCODING),
                attachment=lane,
            )
            _query_until_matches(session, key, payload)

            fixture.create_buffer_session()
            session_generation = uuid.UUID(
                "6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"
            )
            buffer_key = (
                f"{fixture.prefix}/buffers/{fixture.session_id}/durable/fence/raw"
            )
            buffer_payload = b"raw-fence-payload"
            session.put(
                buffer_key,
                buffer_payload,
                encoding=zenoh.Encoding("zenoh/bytes"),
                attachment=lane,
            )
            deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
            buffer_replies: list[support.WireSample] = []
            while not buffer_replies and time.monotonic() < deadline:
                buffer_replies = _read_exact_reply(session, buffer_key)
            assert len(buffer_replies) == 1
            support.assert_wire_sample(
                buffer_replies[0],
                expected_key=buffer_key,
                expected_payload=buffer_payload,
                expected_encoding="zenoh/bytes",
            )

            buffer_token = uuid.uuid4()
            buffer_marker = (
                f"{fixture.prefix}/meta/fence/buffer/{fixture.session_id}/"
                f"{session_generation}/durable/{publisher}/applied/1"
            )
            session.put(
                buffer_marker,
                b"\x01",
                encoding=zenoh.Encoding("zenoh/bytes;sitos.v1.fence"),
                attachment=bytes([1]) + buffer_token.bytes,
            )
            buffer_ack_key = f"{fixture.prefix}/meta/ack/{buffer_token}"
            buffer_ack: list[support.WireSample] = []
            deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
            while not buffer_ack and time.monotonic() < deadline:
                buffer_ack = _read_exact_reply(session, buffer_ack_key)
            assert len(buffer_ack) == 1
            assert buffer_ack[0].payload[1:4] == bytes([3, 0, 1])

            gap_key = (
                f"{fixture.prefix}/buffers/{fixture.session_id}/durable/fence/gap"
            )
            session.put(
                gap_key,
                b"must-not-apply",
                encoding=zenoh.Encoding("zenoh/bytes"),
                attachment=bytes([1]) + publisher.bytes + struct.pack("<Q", 3),
            )
            gap_token = uuid.uuid4()
            gap_marker = (
                f"{fixture.prefix}/meta/fence/buffer/{fixture.session_id}/"
                f"{session_generation}/durable/{publisher}/applied/3"
            )
            session.put(
                gap_marker,
                b"\x01",
                encoding=zenoh.Encoding("zenoh/bytes;sitos.v1.fence"),
                attachment=bytes([1]) + gap_token.bytes,
            )
            gap_ack_key = f"{fixture.prefix}/meta/ack/{gap_token}"
            gap_ack: list[support.WireSample] = []
            deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
            while not gap_ack and time.monotonic() < deadline:
                gap_ack = _read_exact_reply(session, gap_ack_key)
            assert len(gap_ack) == 1
            assert gap_ack[0].payload[1:4] == bytes([3, 9, 1])
            assert struct.unpack_from("<Q", gap_ack[0].payload, 20)[0] == 2
            assert _read_exact_reply(session, gap_key) == []

            cache_token = uuid.uuid4()
            cache_marker = (
                f"{fixture.prefix}/meta/fence/cache/s1/{receiver}/{publisher}/0"
            )
            session.put(
                cache_marker,
                b"\x01",
                encoding=zenoh.Encoding("zenoh/bytes;sitos.v1.fence"),
                attachment=bytes([1]) + cache_token.bytes,
            )
            cache_ack = f"{fixture.prefix}/meta/ack/{cache_token}"
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                assert _read_exact_reply(session, cache_ack) == []

            missing_token = uuid.uuid4()
            missing_marker = (
                f"{fixture.prefix}/meta/fence/buffer/missing/{receiver}/durable/"
                f"{publisher}/applied/0"
            )
            session.put(
                missing_marker,
                b"\x01",
                encoding=zenoh.Encoding("zenoh/bytes;sitos.v1.fence"),
                attachment=bytes([1]) + missing_token.bytes,
            )
            missing_ack = f"{fixture.prefix}/meta/ack/{missing_token}"
            replies: list[support.WireSample] = []
            deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
            while not replies and time.monotonic() < deadline:
                replies = _read_exact_reply(session, missing_ack)
            assert len(replies) == 1
            result = replies[0]
            assert result.key == missing_ack
            assert result.encoding == "zenoh/bytes;sitos.v1.ack"
            assert len(result.payload) == 32
            version, kind, status, durability = result.payload[:4]
            assert (version, kind, status, durability) == (1, 3, 1, 1)
            assert struct.unpack_from("<I", result.payload, 4)[0] == 0
            assert struct.unpack_from("<I", result.payload, 8)[0] == 0xFFFFFFFF
            assert struct.unpack_from("<Q", result.payload, 12)[0] == 0
            assert struct.unpack_from("<Q", result.payload, 20)[0] == 0xFFFFFFFFFFFFFFFF


def test_raw_delete_with_valid_ack_attachment_is_rejected() -> None:
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            key = f"{fixture.prefix}/base/raw/delete-valid-ack"
            payload = _encode_dp(41.0)
            _put_until_query_matches(session, key, payload)

            token = uuid.uuid4()
            session.delete(key, attachment=bytes([1]) + token.bytes)
            ack_key = f"{fixture.prefix}/meta/ack/{token}"
            deadline = time.monotonic() + SCENARIO_TIMEOUT_SECONDS
            replies: list[support.WireSample] = []
            while not replies and time.monotonic() < deadline:
                replies = _read_exact_reply(session, ack_key)
            assert len(replies) == 1, "attached Delete was not processed as an invalid ACK operation"
            _query_until_matches(session, key, payload)


def test_raw_delete_with_malformed_ack_attachment_is_rejected() -> None:
    with support.FixtureProcess() as fixture:
        with fixture.open_raw_session() as session:
            key = f"{fixture.prefix}/base/raw/delete-malformed-ack"
            payload = _encode_dp(42.0)
            _put_until_query_matches(session, key, payload)

            observed_delete = threading.Event()

            def observe(sample: zenoh.Sample) -> None:
                if sample.kind == zenoh.SampleKind.DELETE:
                    observed_delete.set()

            subscriber = session.declare_subscriber(key, observe)
            try:
                # Exact 17-byte attachments are reserved for ADR-0028. Keep
                # this malformed-ACK regression disjoint from ADR-0029's
                # schema-v1 wrong-length Fence candidate classification.
                session.delete(key, attachment=bytes([1]) + bytes(16))
                assert observed_delete.wait(SCENARIO_TIMEOUT_SECONDS), (
                    "raw subscriber did not observe the malformed attached Delete"
                )
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    replies = _read_exact_reply(session, key)
                    assert len(replies) == 1, "malformed attached Delete reached StorageEngine::Delete"
                    support.assert_wire_sample(
                        replies[0],
                        expected_key=key,
                        expected_payload=payload,
                        expected_encoding=support.CANONICAL_SITOS_ENCODING,
                    )
            finally:
                subscriber.undeclare()


def test_fixture_reports_transport_startup_failure() -> None:
    executable = os.environ["SITOS_RAW_ZENOH_FIXTURE"]
    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        occupied_port = occupied.getsockname()[1]
        result = subprocess.run(
            [
                executable,
                f"sitos/interop_diagnostic_{os.getpid()}",
                str(occupied_port),
            ],
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    assert result.returncode == 3
    assert "OpenZenohTransport failed: status=" in result.stderr
    assert ", cause=" in result.stderr
    assert "{mode:" not in result.stderr


def test_cleanup_failure_preserves_primary_without_exception_notes(capsys) -> None:
    class LegacyError(RuntimeError):
        add_note = None

    fixture = object.__new__(support.FixtureProcess)

    def fail_cleanup() -> None:
        raise AssertionError("cleanup diagnostic")

    fixture.close = fail_cleanup
    with pytest.raises(LegacyError, match="primary failure"):
        try:
            raise LegacyError("primary failure")
        except LegacyError as primary:
            fixture.__exit__(LegacyError, primary, primary.__traceback__)
            raise
    assert "fixture cleanup failed: cleanup diagnostic" in capsys.readouterr().err


def test_forced_cleanup_terminates_kills_and_preserves_primary(capsys) -> None:
    process = Mock()
    process.stdin = Mock()
    process.returncode = None
    wait_attempt = 0

    def wait(timeout: float) -> int:
        nonlocal wait_attempt
        wait_attempt += 1
        if wait_attempt <= 2:
            raise subprocess.TimeoutExpired("fixture", timeout)
        process.returncode = -9
        return process.returncode

    process.poll.return_value = None
    process.wait.side_effect = wait

    fixture = object.__new__(support.FixtureProcess)
    fixture._process = process
    fixture._stdout_reader = Mock()
    fixture._stderr_reader = Mock()
    fixture._stderr_lock = threading.Lock()
    fixture.stderr_output = []
    fixture.output = []
    fixture._readline = Mock(return_value="STOPPED")

    class LegacyError(RuntimeError):
        add_note = None

    with pytest.raises(LegacyError, match="primary failure"):
        with fixture:
            raise LegacyError("primary failure")

    assert process.method_calls == [
        call.poll(),
        call.stdin.write("STOP\n"),
        call.stdin.flush(),
        call.wait(timeout=5),
        call.terminate(),
        call.wait(timeout=5),
        call.kill(),
        call.wait(timeout=5),
        call.stdin.close(),
    ]
    fixture._readline.assert_called_once_with(5.0)
    fixture._stdout_reader.join.assert_called_once_with(timeout=1)
    fixture._stderr_reader.join.assert_called_once_with(timeout=1)
    assert "fixture required forced termination" in capsys.readouterr().err


def test_fixture_retries_a_port_bind_race(monkeypatch) -> None:
    real_socket = socket.socket
    with real_socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        occupied_port = occupied.getsockname()[1]
        first_probe = True

        class OccupiedPortProbe:
            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback) -> None:
                pass

            def bind(self, endpoint) -> None:
                pass

            def getsockname(self):
                return ("127.0.0.1", occupied_port)

        def probe_socket(*args, **kwargs):
            nonlocal first_probe
            if first_probe:
                first_probe = False
                return OccupiedPortProbe()
            return real_socket(*args, **kwargs)

        monkeypatch.setattr(support.socket, "socket", probe_socket)
        with support.FixtureProcess():
            pass
