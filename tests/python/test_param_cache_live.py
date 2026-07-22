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


def _eventually_get(cache, key: str, timeout: float = 5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return cache.get(key)
        except sitos.NotFoundError:
            threading.Event().wait(0.01)
    raise AssertionError(f"timed out waiting for cache key {key}")


def test_attach_validation_detach_and_reattach(live_cache_fixture) -> None:
    _, config = live_cache_fixture
    cache = _new_cache(config)
    try:
        cache.detach()
        with pytest.raises(ValueError):
            cache.attach("bad/session")
        cache.attach("unknown_but_valid")
        assert cache.contains("missing") is False
        with pytest.raises(ValueError):
            cache.attach("s1")
        cache.detach()
        cache.attach("s1")
        cache.detach()
        cache.attach("s1")
    finally:
        cache.close()


def test_attach_buffers_controlled_concurrent_delta(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    result: list[object] = []
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
        assert _readline_with_timeout(process.stdout) == "SNAPSHOT_RELEASED"
        attach.join(timeout=5)
        assert not attach.is_alive()
        assert result == [None]
        assert _eventually_get(cache, "during_attach") == 7
    finally:
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
        cache.put_batch(values)
        for key, expected in values.items():
            assert cache.get(key) == expected
        assert cache.get("value/dp", type=int) == 3
        with pytest.raises(sitos.TypeMismatchError):
            cache.get("value/str", type=bytes)

        sentinel = object()
        assert cache.get("value/missing", default=sentinel) is sentinel
        with pytest.raises(ValueError):
            cache.get("bad key", default=sentinel)

        cache.put_batch([("value/dup", 1), ("value/dup", 2)])
        assert cache.get("value/dup") == 2
        _send(process, "BATCH_STATUS")
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
        cache.put("peer_value", 41)
        assert cache.get("peer_value") == 41
        _send(process, "WAIT_PEER peer_value 41")
        assert _readline_with_timeout(process.stdout) == "PEER_OBSERVED peer_value 41"
        _send(process, "CHECK_ISOLATION peer_value")
        assert _readline_with_timeout(process.stdout) == "ISOLATED peer_value"
    finally:
        cache.close()


def test_terminal_close_waits_for_admitted_attach_and_releases_gil(live_cache_fixture) -> None:
    process, config = live_cache_fixture
    assert process.stdout is not None
    cache = _new_cache(config)
    attach_result: list[object] = []
    close_done = threading.Event()

    _send(process, "ARM_SNAPSHOT")
    assert _readline_with_timeout(process.stdout) == "ARMED"
    attach = threading.Thread(target=lambda: attach_result.append(cache.attach("s1")), daemon=True)
    attach.start()
    assert _readline_with_timeout(process.stdout) == "SNAPSHOT_ENTERED"

    closer = threading.Thread(target=lambda: (cache.close(), close_done.set()), daemon=True)
    closer.start()
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

    assert not close_done.is_set()
    progress: list[bool] = []
    worker = threading.Thread(target=lambda: progress.append(True))
    worker.start()
    worker.join(timeout=2)
    assert progress == [True]

    _send(process, "RELEASE_SNAPSHOT")
    assert _readline_with_timeout(process.stdout) == "SNAPSHOT_RELEASED"
    attach.join(timeout=5)
    closer.join(timeout=5)
    assert not attach.is_alive()
    assert not closer.is_alive()
    assert close_done.is_set()
    assert len(attach_result) == 1
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.attach("s1")
    cache.close()
    cache.detach()
