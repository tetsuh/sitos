"""Raw Zenoh interoperability contracts for mixed Session buffer routes."""

from __future__ import annotations

import threading

import pytest
import zenoh

import raw_zenoh_test_support as support

QUERY_TIMEOUT_SECONDS = 1.0


def _query(session: zenoh.Session, key: str) -> list[support.WireSample]:
    replies: list[support.WireSample] = []
    for reply in session.get(key, timeout=QUERY_TIMEOUT_SECONDS):
        assert reply.err is None, f"query failed: {reply.err}"
        assert reply.ok is not None
        replies.append(support.copy_sample(reply.ok))
    return replies


def _wait_for_payload(session: zenoh.Session, key: str, payload: bytes) -> None:
    replies = _query(session, key)
    assert len(replies) == 1, f"buffer value was not queryable: {key}"
    support.assert_wire_sample(
        replies[0],
        expected_key=key,
        expected_payload=payload,
        expected_encoding="zenoh/bytes",
    )


def test_raw_zenoh_client_can_use_mixed_session_buffers() -> None:
    with support.FixtureProcess() as fixture:
        session_ids = {
            mode: fixture.create_buffer_session(f"{fixture.session_id}_{mode}", mode)
            for mode in ("none", "durable", "ephemeral", "both")
        }
        with fixture.open_raw_session() as session:
            marker_observed = threading.Event()
            live_observed = threading.Event()
            observed_samples: list[support.WireSample] = []
            live_key = f"{fixture.prefix}/buffers/{session_ids['both']}/ephemeral/live"
            marker_key = f"{fixture.prefix}/buffers/{session_ids['both']}/ephemeral/marker"

            def observe(sample: zenoh.Sample) -> None:
                copied = support.copy_sample(sample)
                observed_samples.append(copied)
                if copied.key == marker_key:
                    marker_observed.set()
                if copied.key == live_key:
                    live_observed.set()

            subscriber = session.declare_subscriber(
                f"{fixture.prefix}/buffers/{session_ids['both']}/ephemeral/**", observe
            )
            try:
                session.put(marker_key, b"marker", encoding=zenoh.Encoding("zenoh/bytes"))
                assert marker_observed.wait(5.0)
                for mode, sid in session_ids.items():
                    durable_key = f"{fixture.prefix}/buffers/{sid}/durable/official"
                    ephemeral_key = f"{fixture.prefix}/buffers/{sid}/ephemeral/preview"
                    session.put(
                        durable_key,
                        mode.encode(),
                        encoding=zenoh.Encoding("zenoh/bytes"),
                    )
                    session.put(
                        durable_key,
                        mode.encode(),
                        encoding=zenoh.Encoding("zenoh/bytes"),
                    )
                    session.put(
                        durable_key,
                        b"conflicting-bytes",
                        encoding=zenoh.Encoding("zenoh/bytes"),
                    )
                    session.put(
                        ephemeral_key,
                        b"preview",
                        encoding=zenoh.Encoding("zenoh/bytes"),
                    )
                durable_replies = {
                    mode: _query(
                        session,
                        f"{fixture.prefix}/buffers/{sid}/durable/official",
                    )
                    for mode, sid in session_ids.items()
                }
                assert len(durable_replies["none"]) == 0
                assert len(durable_replies["durable"]) == 1
                assert len(durable_replies["ephemeral"]) == 0
                assert len(durable_replies["both"]) == 1
                for mode in ("durable", "both"):
                    support.assert_wire_sample(
                        durable_replies[mode][0],
                        expected_key=f"{fixture.prefix}/buffers/{session_ids[mode]}/durable/official",
                        expected_payload=mode.encode(),
                        expected_encoding="zenoh/bytes",
                    )
                for mode, sid in session_ids.items():
                    assert _query(
                        session,
                        f"{fixture.prefix}/buffers/{sid}/ephemeral/preview",
                    ) == []
                invalid_key = f"{fixture.prefix}/buffers/{session_ids['both']}/durable/invalid"
                session.put(invalid_key, b"invalid", encoding=zenoh.Encoding("zenoh/bytes;sitos.v1"))
                assert _query(session, invalid_key) == []
                empty_key = f"{fixture.prefix}/buffers/{session_ids['both']}/durable/empty"
                session.put(empty_key, b"", encoding=zenoh.Encoding("zenoh/bytes"))
                empty_replies = _query(session, empty_key)
                assert len(empty_replies) == 1
                support.assert_wire_sample(
                    empty_replies[0],
                    expected_key=empty_key,
                    expected_payload=b"",
                    expected_encoding="zenoh/bytes",
                )
                session.put(
                    live_key,
                    b"live-preview",
                    encoding=zenoh.Encoding("zenoh/bytes"),
                )
                assert live_observed.wait(5.0)
                live_samples = [sample for sample in observed_samples if sample.key == live_key]
                assert live_samples
                support.assert_wire_sample(
                    live_samples[-1],
                    expected_key=live_key,
                    expected_payload=b"live-preview",
                    expected_encoding="zenoh/bytes",
                )
            finally:
                subscriber.undeclare()


def test_raw_zenoh_durable_late_join_preserves_distinct_keys() -> None:
    with support.FixtureProcess() as fixture:
        fixture.create_buffer_session()
        with fixture.open_raw_session() as first:
            for suffix, payload in (("a", b"a"), ("b", b"b")):
                first.put(
                    f"{fixture.prefix}/buffers/{fixture.session_id}/durable/{suffix}",
                    payload,
                    encoding=zenoh.Encoding("zenoh/bytes"),
                )
            _wait_for_payload(
                first,
                f"{fixture.prefix}/buffers/{fixture.session_id}/durable/a",
                b"a",
            )
        with fixture.open_raw_session() as late:
            replies = _query(late, f"{fixture.prefix}/buffers/{fixture.session_id}/durable/**")
            assert {reply.key for reply in replies} == {
                f"{fixture.prefix}/buffers/{fixture.session_id}/durable/a",
                f"{fixture.prefix}/buffers/{fixture.session_id}/durable/b",
            }
            for reply in replies:
                assert reply.encoding == "zenoh/bytes"
                assert reply.payload == reply.key.rsplit("/", maxsplit=1)[-1].encode()


def test_raw_zenoh_buffer_interop_fixture_boundaries() -> None:
    import importlib.util
    from pathlib import Path

    validator_path = Path(__file__).parents[2] / "scripts" / "check_wheel.py"
    spec = importlib.util.spec_from_file_location("sitos_check_wheel", validator_path)
    assert spec is not None and spec.loader is not None
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    for member in (
        "sitos/sitos_raw_zenoh_buffer_fixture",
        "sitos/sitos_raw_zenoh_buffer_fixture.exe",
    ):
        with pytest.raises(RuntimeError, match="forbidden wheel entry"):
            validator.validate_wheel_members([member])

    with support.FixtureProcess() as fixture:
        fixture.create_buffer_session()
        fixture.command(
            f"CREATE_BUFFER_SESSION {fixture.session_id} invalid",
            "ERROR CREATE_BUFFER_SESSION invalid mode",
        )
        with fixture.open_raw_session() as session:
            closed_key = f"{fixture.prefix}/buffers/{fixture.session_id}/durable/closed"
            session.put(closed_key, b"before-close", encoding=zenoh.Encoding("zenoh/bytes"))
            _wait_for_payload(session, closed_key, b"before-close")
        fixture.close_session()
        with fixture.open_raw_session() as session:
            assert _query(session, closed_key) == []
