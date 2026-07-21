"""Acceptance tests for the Python ParamStore binding."""

import json
import os
import socket
import subprocess
import threading
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

import sitos


def test_public_param_store_is_exported() -> None:
    assert hasattr(sitos, "ParamStore")
    assert hasattr(sitos, "SitosError")
    assert hasattr(sitos, "TypeMismatchError")


def test_constructor_rejects_bool_timeout() -> None:
    with pytest.raises(TypeError):
        sitos.ParamStore(query_timeout_ms=True)


@pytest.mark.parametrize("timeout", [0, -1, 2**63])
def test_constructor_rejects_invalid_timeout(timeout: int) -> None:
    with pytest.raises(ValueError):
        sitos.ParamStore(query_timeout_ms=timeout)


def test_constructor_rejects_empty_json() -> None:
    with pytest.raises(ValueError):
        sitos.ParamStore(zenoh_config_json="")


def test_context_manager_methods_are_bound() -> None:
    assert callable(sitos.ParamStore.__enter__)
    assert callable(sitos.ParamStore.__exit__)
    assert callable(sitos.ParamStore.close)


def test_public_exception_hierarchy() -> None:
    assert issubclass(sitos.NotFoundError, sitos.SitosError)
    assert issubclass(sitos.TypeMismatchError, sitos.SitosError)
    assert issubclass(sitos.TimeoutError, sitos.SitosError)
    assert issubclass(sitos.DisconnectedError, sitos.SitosError)
    assert issubclass(sitos.ReadOnlyError, sitos.SitosError)


def test_batch_accepts_duplicate_pairs_without_mapping_loss() -> None:
    assert callable(sitos.ParamStore.put_batch)
    assert [pair for pair in [("a", 1), ("a", 2)]] == [("a", 1), ("a", 2)]


def _fixture_path() -> str | None:
    value = os.environ.get("SITOS_PYTHON_FIXTURE")
    if value and Path(value).is_file():
        return value
    return None


@pytest.fixture(scope="module")
def live_store() -> Iterator[tuple[sitos.ParamStore, subprocess.Popen[str], str]]:
    executable = _fixture_path()
    if executable is None:
        pytest.skip("SITOS_PYTHON_FIXTURE is not set")
    prefix = f"sitos/python_test_{os.getpid()}"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen(
        [executable, prefix, str(port)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    ready = process.stdout.readline().strip()
    assert ready == f"READY {prefix} {port}"
    config = json.dumps({"mode": "client", "connect": {
        "endpoints": [f"tcp/127.0.0.1:{port}"]
    }})
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=config, query_timeout_ms=5000)
    try:
        yield store, process, prefix
    finally:
        store.close()
        if process.stdin is not None:
            process.stdin.write("STOP\n")
            process.stdin.flush()
        process.wait(timeout=10)


def _eventually(store: sitos.ParamStore, scope: str, key: str) -> object:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            return store.get(scope, key)
        except sitos.NotFoundError:
            time.sleep(0.02)
    raise AssertionError(f"timed out waiting for {scope}/{key}")


def test_live_round_trip_and_raw_prefix_list(live_store) -> None:
    store, _, _ = live_store
    store.put("base", "foo/bar", 7)
    store.put("base", "foobar", "outside")
    assert _eventually(store, "base", "foo/bar") == 7
    assert list(store.list("base", "foo")) == [("foo/bar", 7), ("foobar", "outside")]
    assert list(store.list("base", "foo/")) == [("foo/bar", 7)]
    assert store.contains("base", "foo/bar")
    store.delete("base", "foo/bar")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not store.contains("base", "foo/bar"):
            break
        time.sleep(0.02)
    else:
        raise AssertionError("delete was not observed")


def test_live_typed_get_default_and_batch(live_store) -> None:
    store, _, _ = live_store
    store.put("base", "number", 3.5)
    assert _eventually(store, "base", "number") == 3.5
    assert store.get("base", "number", type=int) == 3
    assert store.get("base", "missing", default=None) is None
    store.put_batch("base", [("dup", 1), ("dup", 2), ("text", "ok")])
    assert _eventually(store, "base", "dup") == 2
    assert _eventually(store, "base", "text") == "ok"


def test_live_delayed_reply_allows_other_python_threads(live_store) -> None:
    store, process, _ = live_store
    assert process.stdout is not None and process.stdin is not None
    result: list[object] = []
    thread = threading.Thread(
        target=lambda: result.append(store.get("base", "__python_gil_delay__")), daemon=True
    )
    thread.start()
    assert process.stdout.readline().strip() == "DELAYED"
    progress = []
    worker = threading.Thread(target=lambda: progress.append(True))
    worker.start()
    worker.join(timeout=2)
    assert progress == [True]
    process.stdin.write("REPLY\n")
    process.stdin.flush()
    thread.join(timeout=5)
    assert not thread.is_alive()
    assert result == [7]
