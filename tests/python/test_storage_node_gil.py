"""Source-test-only deterministic GIL boundary checks."""

from __future__ import annotations

import threading
import time

import pytest

import sitos


@pytest.fixture
def gil_control():
    control = sitos._sitos
    if not hasattr(control, "_gil_test_arm"):
        pytest.skip("source-only GIL test support is disabled")
    control._gil_test_reset()
    yield control
    control._gil_test_reset()


def _assert_heartbeat_while_blocked(control, boundary: str, operation) -> None:
    control._gil_test_arm(boundary)
    completed = threading.Event()
    errors: list[BaseException] = []

    def invoke() -> None:
        try:
            operation()
        except BaseException as error:
            errors.append(error)
        finally:
            completed.set()

    worker = threading.Thread(target=invoke)
    worker.start()
    assert control._gil_test_wait(boundary, 5000)

    heartbeat = 0
    deadline = time.monotonic() + 1.0
    while heartbeat < 100 and time.monotonic() < deadline:
        heartbeat += 1
        time.sleep(0.001)
    assert heartbeat >= 100
    control._gil_test_release(boundary)
    worker.join(timeout=5)
    assert completed.is_set()
    assert errors == []


def test_constructor_releases_gil_at_source_boundary(gil_control) -> None:
    engine = sitos.InMemoryEngine()
    created: list[sitos.StorageNode] = []
    _assert_heartbeat_while_blocked(
        gil_control,
        "constructor",
        lambda: created.append(
            sitos.StorageNode(engine, prefix="sitos/python_node_gil_constructor")
        ),
    )
    created[0].stop()


def test_create_session_releases_gil_at_source_boundary(gil_control) -> None:
    node = sitos.StorageNode(
        sitos.InMemoryEngine(), prefix="sitos/python_node_gil_create"
    )
    _assert_heartbeat_while_blocked(
        gil_control,
        "create_session",
        lambda: node.create_session("gil_session"),
    )
    node.stop()


def test_stop_releases_gil_at_source_boundary(gil_control) -> None:
    node = sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_gil_stop")
    _assert_heartbeat_while_blocked(gil_control, "stop", node.stop)
