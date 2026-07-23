"""Live acceptance tests for the Python ParamCache binding."""

import json
import os
import queue
import socket
import subprocess
import threading
import time
from collections.abc import Iterator
from pathlib import Path

import numpy as np
import pytest

import sitos


_FIXTURE = os.environ.get("SITOS_PYTHON_CACHE_FIXTURE")
pytestmark = pytest.mark.skipif(
    not _FIXTURE or not Path(_FIXTURE).is_file(),
    reason="SITOS_PYTHON_CACHE_FIXTURE is not set",
)


def _readline_with_timeout(stream, timeout: float = 10.0) -> str:
    lines: queue.Queue[str] = queue.Queue(maxsize=1)

    def read() -> None:
        lines.put(stream.readline())

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    reader.join(timeout)
    if reader.is_alive():
        raise AssertionError(f"fixture did not produce a line within {timeout:g} seconds")
    return lines.get_nowait().strip()


def _send(process: subprocess.Popen[str], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write(f"{command}\n")
    process.stdin.flush()


def _stop_fixture_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        try:
            _send(process, "STOP")
        except BrokenPipeError:
            pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


@pytest.fixture(scope="module")
def live_cache_fixture() -> Iterator[tuple[subprocess.Popen[str], dict[str, object]]]:
    assert _FIXTURE is not None
    prefix = f"sitos/python_cache_{os.getpid()}"
    sid = "s1"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen(
        [_FIXTURE, prefix, str(port), sid],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        assert process.stdout is not None
        assert _readline_with_timeout(process.stdout) == f"READY {prefix} {port} {sid}"
        config = {
            "prefix": prefix,
            "zenoh_config_json": json.dumps(
                {"mode": "client", "connect": {"endpoints": [f"tcp/127.0.0.1:{port}"]}}
            ),
            "query_timeout_ms": 5000,
        }
        yield process, config
    finally:
        _stop_fixture_process(process)


def _new_cache(config: dict[str, object]):
    return sitos.ParamCache(**config)


def test_attach_validation_detach_and_reattach(live_cache_fixture) -> None:
    _, config = live_cache_fixture
    cache = _new_cache(config)
    try:
        cache.detach()
        detached_operations = [
            lambda: cache.get("missing"),
            lambda: cache.contains("missing"),
            lambda: list(cache.items()),
            lambda: cache.put("detached", 1),
            lambda: cache.put_batch([]),
        ]
        for operation in detached_operations:
            with pytest.raises(ValueError, match="detached"):
                operation()

        with pytest.raises(ValueError):
            cache.attach("bad/session")
        cache.attach("unknown_but_valid")
        assert cache.contains("missing") is False
        assert cache.get("missing", default=None) is None
        with pytest.raises(ValueError):
            cache.attach("s1")
        cache.detach()
        cache.attach("s1")
        cache.detach()
        cache.attach("s1")
    finally:
        cache.close()


def test_attach_observes_controlled_concurrent_delta(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    result: list[object] = []
    released = False
    try:
        _send(process, "ARM_SNAPSHOT")
        assert _readline_with_timeout(process.stdout) == "ARMED"
        attach = threading.Thread(target=lambda: result.append(cache.attach("s1")), daemon=True)
        attach.start()
        assert _readline_with_timeout(process.stdout) == "SNAPSHOT_ENTERED"

        progress: list[bool] = []
        worker = threading.Thread(target=lambda: progress.append(True))
        worker.start()
        worker.join(timeout=2)
        assert progress == [True]

        _send(process, "PUT_DURING_ATTACH")
        assert _readline_with_timeout(process.stdout) == "DELTA_SUBMITTED"
        _send(process, "RELEASE_SNAPSHOT")
        released = True
        assert _readline_with_timeout(process.stdout) == "SNAPSHOT_RELEASED"
        attach.join(timeout=5)
        assert not attach.is_alive()
        assert result == [None]
        assert cache.get("during_attach") == 7
    finally:
        if not released:
            _send(process, "RELEASE_SNAPSHOT")
            _readline_with_timeout(process.stdout)
        cache.close()


def test_values_typed_get_default_batch_and_owned_items(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    try:
        cache.attach("s1")
        values = {
            "value/bool": True,
            "value/s64": -7,
            "value/dp": 3.5,
            "value/str": "text",
            "value/bytes": b"bytes",
        }
        _send(process, "BATCH_STATUS")
        initial_batch_count = int(_readline_with_timeout(process.stdout).split()[1])
        cache.put_batch(values)
        _send(process, f"WAIT_BATCH {initial_batch_count}")
        first_status = _readline_with_timeout(process.stdout)
        assert int(first_status.split()[1]) == initial_batch_count + 1
        assert first_status.split()[2] == "5"
        for key, expected in values.items():
            assert cache.get(key) == expected
        assert cache.get("value/dp", type=int) == 3
        with pytest.raises(sitos.TypeMismatchError):
            cache.get("value/str", type=bytes)

        class IntSubclass(int):
            pass

        with pytest.raises(TypeError, match="type must be"):
            cache.get("value/s64", type=IntSubclass)

        sentinel = object()
        assert cache.get("value/missing", default=sentinel) is sentinel
        assert cache.get("value/missing", default=None) is None
        with pytest.raises(ValueError):
            cache.get("bad key", default=sentinel)

        _send(process, "BATCH_STATUS")
        before_count = int(_readline_with_timeout(process.stdout).split()[1])
        cache.put_batch([("value/dup", 1), ("value/dup", 2)])
        assert cache.get("value/dup") == 2
        _send(process, f"WAIT_BATCH {before_count}")
        status = _readline_with_timeout(process.stdout)
        assert status.endswith(" 2 1 2")
        before_count = int(status.split()[1])
        cache.put_batch([])
        _send(process, "BATCH_STATUS")
        assert int(_readline_with_timeout(process.stdout).split()[1]) == before_count

        cache.put("foo/bar", 7)
        cache.put("foobar", "outside")
        assert list(cache.items("foo")) == [("foo/bar", 7), ("foobar", "outside")]
        rows = cache.items("foo/")
        cache.detach()
        assert list(rows) == [("foo/bar", 7)]
    finally:
        cache.close()


def test_numpy_get_array_is_zero_copy_and_owned_after_overwrite(live_cache_fixture) -> None:
    _, config = live_cache_fixture
    cache = _new_cache(config)
    try:
        cache.attach("s1")
        source = np.array([1, 2, 3, 4], dtype=np.int16)
        cache.put("array", source)
        first = cache.get_array("array", dtype=np.int16)
        second = cache.get_array("array", dtype=np.int16)
        assert first.dtype == np.dtype(np.int16)
        assert first.shape == (4,)
        assert not first.flags.writeable
        assert first.__array_interface__["data"][0] == second.__array_interface__["data"][0]
        assert np.shares_memory(first, second)
        with pytest.raises(ValueError):
            first[0] = 99

        replacement = np.array([5, 6, 7, 8], dtype=np.int16)
        cache.put("array", replacement)
        updated = cache.get_array("array", dtype=np.int16)
        assert first.tolist() == [1, 2, 3, 4]
        assert updated.tolist() == [5, 6, 7, 8]
        cache.detach()
        assert first.tolist() == [1, 2, 3, 4]
    finally:
        cache.close()


def test_invalid_inputs_leave_existing_local_state_unchanged(live_cache_fixture) -> None:
    _, config = live_cache_fixture
    cache = _new_cache(config)
    try:
        cache.attach("s1")
        cache.put("stable", 11)
        with pytest.raises(ValueError, match="two items"):
            cache.put_batch([("stable", 12, "extra")])
        with pytest.raises(TypeError, match="two-item pairs"):
            cache.put_batch(["not-a-pair"])
        with pytest.raises(TypeError):
            cache.put("stable", bytearray(b"unsupported"))
        with pytest.raises(OverflowError):
            cache.put("stable", 2**63)
        with pytest.raises(ValueError):
            cache.items("bad prefix")
        assert cache.get("stable") == 11
    finally:
        cache.close()


def test_write_reaches_cpp_peer_without_base_or_other_session_mutation(
    live_cache_fixture,
) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    try:
        cache.attach("s1")
        _send(process, "PEER_COUNT")
        previous_callback_count = int(_readline_with_timeout(process.stdout).split()[1])
        cache.put("peer_value", 41)
        assert cache.get("peer_value") == 41
        _send(process, f"WAIT_PEER {previous_callback_count} peer_value 41")
        assert _readline_with_timeout(process.stdout) == "PEER_OBSERVED peer_value 41"
        _send(process, "CHECK_ISOLATION peer_value")
        assert _readline_with_timeout(process.stdout) == "ISOLATED peer_value"
    finally:
        cache.close()


def test_detach_waits_for_admitted_attach_and_releases_gil(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    attach_result: list[object] = []
    detach_done = threading.Event()
    released = False

    try:
        _send(process, "ARM_SNAPSHOT")
        assert _readline_with_timeout(process.stdout) == "ARMED"
        attach = threading.Thread(
            target=lambda: attach_result.append(cache.attach("s1")), daemon=True
        )
        attach.start()
        assert _readline_with_timeout(process.stdout) == "SNAPSHOT_ENTERED"

        detacher = threading.Thread(target=lambda: (cache.detach(), detach_done.set()), daemon=True)
        detacher.start()
        progress: list[bool] = []
        worker = threading.Thread(target=lambda: progress.append(True))
        worker.start()
        worker.join(timeout=2)
        assert progress == [True]
        assert not detach_done.is_set()

        _send(process, "RELEASE_SNAPSHOT")
        released = True
        assert _readline_with_timeout(process.stdout) == "SNAPSHOT_RELEASED"
        attach.join(timeout=5)
        detacher.join(timeout=5)
        assert not attach.is_alive()
        assert not detacher.is_alive()
        assert attach_result == [None]
        assert detach_done.is_set()
        with pytest.raises(ValueError, match="detached"):
            cache.get("during_attach")
    finally:
        if not released:
            _send(process, "RELEASE_SNAPSHOT")
            _readline_with_timeout(process.stdout)
        cache.close()


def test_terminal_close_waits_for_admitted_attach_and_releases_gil(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    attach_result: list[object] = []
    first_close_done = threading.Event()
    second_close_done = threading.Event()

    _send(process, "ARM_SNAPSHOT")
    assert _readline_with_timeout(process.stdout) == "ARMED"
    attach = threading.Thread(target=lambda: attach_result.append(cache.attach("s1")), daemon=True)
    attach.start()
    assert _readline_with_timeout(process.stdout) == "SNAPSHOT_ENTERED"

    first_closer = threading.Thread(
        target=lambda: (cache.close(), first_close_done.set()), daemon=True
    )
    second_closer = threading.Thread(
        target=lambda: (cache.close(), second_close_done.set()), daemon=True
    )
    first_closer.start()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            cache.contains("key")
        except ValueError as error:
            if str(error) == "ParamCache is closed":
                break
        threading.Event().wait(0.01)
    else:
        raise AssertionError("close did not close operation admission")

    second_closer.start()
    assert not first_close_done.is_set()
    assert not second_close_done.is_set()
    progress: list[bool] = []
    worker = threading.Thread(target=lambda: progress.append(True))
    worker.start()
    worker.join(timeout=2)
    assert progress == [True]

    _send(process, "RELEASE_SNAPSHOT")
    assert _readline_with_timeout(process.stdout) == "SNAPSHOT_RELEASED"
    attach.join(timeout=5)
    first_closer.join(timeout=5)
    second_closer.join(timeout=5)
    assert not attach.is_alive()
    assert not first_closer.is_alive()
    assert not second_closer.is_alive()
    assert first_close_done.is_set()
    assert second_close_done.is_set()
    assert len(attach_result) == 1
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.attach("s1")
    cache.close()
    cache.detach()
