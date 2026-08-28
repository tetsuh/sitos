"""Acceptance tests for the Python ParamStore binding."""

import importlib.util
import json
import os
import queue
import socket
import subprocess
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from unittest.mock import Mock

import pytest

import sitos


def _load_check_wheel():
    script = Path(__file__).resolve().parents[2] / "scripts" / "check_wheel.py"
    spec = importlib.util.spec_from_file_location("sitos_check_wheel", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the wheel validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize(
    "member",
    [
        "sitos/sitos_python_param_store_fixture.exe",
        "sitos/sitos_python_param_store_fixture",
    ],
)
def test_wheel_validator_rejects_param_store_fixture_member(member: str) -> None:
    with pytest.raises(RuntimeError, match="forbidden wheel entry"):
        _load_check_wheel().validate_wheel_members([member])


def test_public_param_store_is_exported() -> None:
    assert hasattr(sitos, "ParamStore")
    assert hasattr(sitos, "SitosError")
    assert hasattr(sitos, "TypeMismatchError")


def test_constructor_rejects_positional_arguments() -> None:
    with pytest.raises(TypeError):
        sitos.ParamStore("sitos", "")


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


def test_context_manager_closes_store() -> None:
    with sitos.ParamStore(query_timeout_ms=5000) as store:
        assert isinstance(store, sitos.ParamStore)
    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.contains("base", "key")


def test_context_manager_methods_are_bound() -> None:
    assert callable(sitos.ParamStore.__enter__)
    assert callable(sitos.ParamStore.__exit__)
    assert callable(sitos.ParamStore.close)


def test_closed_store_precedes_put_conversion_and_batch_iteration() -> None:
    store = sitos.ParamStore(query_timeout_ms=5000)
    store.close()

    class ExplodingEntries:
        def __iter__(self):
            raise AssertionError("closed store must not iterate entries")

    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.put("base", "key", object())
    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.put_batch("base", ExplodingEntries())
    store.close()


def test_public_exception_hierarchy() -> None:
    assert issubclass(sitos.NotFoundError, sitos.SitosError)
    assert issubclass(sitos.TypeMismatchError, sitos.SitosError)
    assert issubclass(sitos.TimeoutError, sitos.SitosError)
    assert issubclass(sitos.DisconnectedError, sitos.SitosError)
    assert issubclass(sitos.ReadOnlyError, sitos.SitosError)
    assert issubclass(sitos.OutcomeUnknownError, sitos.SitosError)


def test_batch_rejects_malformed_pairs_before_submission() -> None:
    class MalformedMapping:
        def items(self):
            return [("a", 1, "extra")]

    with sitos.ParamStore(query_timeout_ms=5000) as store:
        with pytest.raises(ValueError, match="two items"):
            store.put_batch("base", [("a",)])
        with pytest.raises(TypeError, match="two-item pairs"):
            store.put_batch("base", ["not-a-pair"])
        with pytest.raises(ValueError, match="two items"):
            store.put_batch("base", MalformedMapping())


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


def test_stop_fixture_does_not_write_to_exited_process() -> None:
    process = Mock()
    process.poll.return_value = 1

    _stop_fixture_process(process)

    process.stdin.write.assert_not_called()
    process.wait.assert_called_once_with(timeout=10)


def test_stop_fixture_preserves_failure_when_stop_pipe_is_broken() -> None:
    process = Mock()
    process.poll.return_value = None
    process.stdin.write.side_effect = BrokenPipeError

    _stop_fixture_process(process)

    process.wait.assert_called_once_with(timeout=10)


def _stop_fixture_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is None and process.stdin is not None:
        try:
            process.stdin.write("STOP\n")
            process.stdin.flush()
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
    store = None
    try:
        assert process.stdout is not None
        ready = _readline_with_timeout(process.stdout)
        assert ready == f"READY {prefix} {port}"
        config = json.dumps({"mode": "client", "connect": {
            "endpoints": [f"tcp/127.0.0.1:{port}"]
        }})
        store = sitos.ParamStore(prefix=prefix, zenoh_config_json=config, query_timeout_ms=5000)
        yield store, process, prefix
    finally:
        if store is not None:
            store.close()
        _stop_fixture_process(process)


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
    store, process, _ = live_store
    assert process.stdout is not None
    store.put("base", "number", 3.5)
    assert _eventually(store, "base", "number") == 3.5
    assert store.get("base", "number", type=int) == 3
    assert store.get("base", "missing", default=None) is None
    store.put_batch("base", [("dup", 1), ("dup", 2)])
    assert _readline_with_timeout(process.stdout) == "BATCH 2 1 2"
    assert _eventually(store, "base", "dup") == 2


def test_live_value_domain_mapping_empty_batch_and_type_mismatch(live_store) -> None:
    numpy = pytest.importorskip("numpy")
    store, _, _ = live_store
    values = {
        "bool": True,
        "s64": -7,
        "dp": 3.5,
        "str": "text",
        "bytes": b"bytes",
    }
    store.put_batch("base", values)
    put_source = numpy.array([1, 2], dtype=numpy.uint16)
    batch_source = numpy.array([3, 4], dtype=numpy.uint16)
    store.put("base", "array", put_source)
    store.put_batch("base", [("array-batch", batch_source)])
    put_source[0] = 99
    batch_source[0] = 99
    for key, expected in values.items():
        assert _eventually(store, "base", key) == expected
    store.put_batch("base", {})
    with pytest.raises(OverflowError):
        store.put("base", "overflow", 2**63)
    assert _eventually(store, "base", "array") == b"\x01\x00\x02\x00"
    assert _eventually(store, "base", "array-batch") == b"\x03\x00\x04\x00"
    with pytest.raises(sitos.TypeMismatchError):
        store.get("base", "str", default=b"fallback", type=bytes)


def test_param_store_ack_options_are_keyword_only_and_strict() -> None:
    store = sitos.ParamStore(query_timeout_ms=5000)
    for method in (sitos.ParamStore.put, sitos.ParamStore.put_batch):
        documentation = method.__doc__ or ""
        assert "ack: object = True" in documentation
        assert "ack_timeout_ms: object = 3000" in documentation
    with pytest.raises(TypeError):
        store.put("base", "key", 1, False)
    with pytest.raises(TypeError):
        store.put_batch("base", [], True)
    with pytest.raises(TypeError, match="ack must be a bool"):
        store.put("base", "key", 1, ack=1)
    with pytest.raises(TypeError, match="ack must be a bool"):
        store.put_batch("base", [], ack=1)
    with pytest.raises(TypeError, match="ack_timeout_ms"):
        store.put("base", "key", 1, ack_timeout_ms=True)
    with pytest.raises(ValueError, match="ack_timeout_ms must be positive"):
        store.put("base", "key", 1, ack_timeout_ms=0)
    with pytest.raises(ValueError, match="ack_timeout_ms must be positive"):
        store.put_batch("base", [], ack_timeout_ms=0)
    for timeout in (2**63, -2**63 - 1):
        for operation in (
            lambda timeout=timeout: store.put("base", "key", 1, ack=False, ack_timeout_ms=timeout),
            lambda timeout=timeout: store.put_batch(
                "base", [], ack=False, ack_timeout_ms=timeout
            ),
        ):
            with pytest.raises(ValueError, match=r"outside the C\+\+ millisecond range"):
                operation()
    with pytest.raises(TypeError):
        store.put_batch("base", [], ack=False, ack_timeout_ms=None)
    for timeout in (1.5, "1"):
        with pytest.raises(TypeError, match="ack_timeout_ms must be an integer"):
            store.put_batch("base", [], ack=False, ack_timeout_ms=timeout)
    store.close()


def test_param_store_submission_only_options_work_without_ack_server() -> None:
    with sitos.ParamStore(query_timeout_ms=5000) as store:
        store.put("base", "submission_only", 1, ack=False, ack_timeout_ms=0)
        store.put_batch("base", [("submission_batch", 2)], ack=False, ack_timeout_ms=-1)


def _test_store_or_skip(mode: str) -> sitos.ParamStore:
    factory = getattr(sitos.ParamStore, "_test_store", None)
    if factory is None:
        pytest.skip("SITOS_PYTHON_TEST_SUPPORT is unavailable")
    return factory(mode)


@pytest.mark.parametrize(
    ("mode", "exception", "message"),
    [
        ("outcome_unknown", sitos.OutcomeUnknownError, "test remote outcome_unknown"),
        ("disconnected", sitos.DisconnectedError, "test remote disconnected"),
    ],
)
def test_source_test_store_ack_status_mapping(mode, exception, message) -> None:
    with _test_store_or_skip(mode) as store:
        for operation in (
            lambda: store.put("base", "key", 1),
            lambda: store.put_batch("base", [("key", 1)]),
        ):
            with pytest.raises(exception, match=message):
                operation()


def test_source_test_store_rejects_unknown_mode() -> None:
    factory = getattr(sitos.ParamStore, "_test_store", None)
    if factory is None:
        pytest.skip("SITOS_PYTHON_TEST_SUPPORT is unavailable")
    with pytest.raises(ValueError, match="unknown ParamStore test mode"):
        factory("typo")


def test_source_test_store_submission_failure_and_missing_ack() -> None:
    with _test_store_or_skip("submit_disconnect") as store:
        with pytest.raises(sitos.DisconnectedError, match="test submission disconnected"):
            store.put("base", "key", 1, ack=False)
        with pytest.raises(sitos.DisconnectedError, match="test submission disconnected"):
            store.put_batch("base", [("key", 1)], ack=False)
    with _test_store_or_skip("timeout") as store:
        with pytest.raises(sitos.TimeoutError, match="no acknowledgement within"):
            store.put("base", "key", 1, ack_timeout_ms=40)
        with pytest.raises(sitos.TimeoutError, match="no acknowledgement within"):
            store.put_batch("base", [("key", 1)], ack_timeout_ms=40)


def test_source_test_store_ack_polling_releases_gil() -> None:
    store = _test_store_or_skip("delayed")
    result: list[BaseException] = []
    progress: list[bool] = []
    control = sitos._sitos
    control._gil_test_arm("param_store_ack")
    writer = threading.Thread(
        target=lambda: _capture_exception(
            result, lambda: store.put("base", "gil", 1, ack_timeout_ms=1000)
        ),
        daemon=True,
    )
    try:
        writer.start()
        assert control._gil_test_wait("param_store_ack", 1000)
        assert writer.is_alive()
        worker = threading.Thread(target=lambda: progress.append(True))
        worker.start()
        worker.join(timeout=1)
        assert not worker.is_alive()
        assert progress == [True]
        control._gil_test_release("param_store_ack")
        writer.join(timeout=2)
        assert not writer.is_alive()
        assert result == []
    finally:
        try:
            control._gil_test_release("param_store_ack")
        except (RuntimeError, ValueError):
            pass
        control._gil_test_reset()
        writer.join(timeout=2)
        store.close()


def _capture_exception(target: list[BaseException], operation) -> None:
    try:
        operation()
    except BaseException as error:  # pragma: no cover - failure diagnostic
        target.append(error)


def test_read_only_scope_maps_to_public_exception() -> None:
    with sitos.ParamStore(query_timeout_ms=5000) as store:
        with pytest.raises(sitos.ReadOnlyError):
            store.put("snap/snapshot", "key", 1)


def test_live_conversion_failure_performs_no_mutation(live_store) -> None:
    store, _, _ = live_store
    with pytest.raises(TypeError):
        store.put("base", "unsupported", object())
    with pytest.raises(sitos.NotFoundError):
        store.get("base", "unsupported")


def test_live_get_type_is_keyword_only_and_exact_builtin(live_store) -> None:
    store, _, _ = live_store
    store.put("base", "typed", 3.5)
    assert _eventually(store, "base", "typed") == 3.5
    with pytest.raises(TypeError):
        store.get("base", "typed", None, int)

    class SpoofType:
        def __eq__(self, other: object) -> bool:
            return other in (bool, int, float, str, bytes)

    with pytest.raises(TypeError, match="type must be None"):
        store.get("base", "typed", type=SpoofType())


def test_list_snapshot_survives_close(live_store) -> None:
    _, process, prefix = live_store
    port = int(process.args[2])
    config = json.dumps({"mode": "client", "connect": {
        "endpoints": [f"tcp/127.0.0.1:{port}"]
    }})
    sibling = sitos.ParamStore(prefix=prefix, zenoh_config_json=config, query_timeout_ms=5000)
    try:
        sibling.put("base", "snapshot/a", True)
        assert _eventually(sibling, "base", "snapshot/a") is True
        rows = sibling.list("base", "snapshot/")
        sibling.close()
        assert list(rows) == [("snapshot/a", True)]
    finally:
        sibling.close()


def test_live_delayed_reply_allows_other_python_threads(live_store) -> None:
    store, process, _ = live_store
    assert process.stdout is not None and process.stdin is not None
    result: list[object] = []
    thread = threading.Thread(
        target=lambda: result.append(store.get("base", "__python_gil_delay__")), daemon=True
    )
    thread.start()
    assert _readline_with_timeout(process.stdout) == "DELAYED"
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


def test_close_returns_during_admitted_delayed_operation(live_store) -> None:
    store, process, _ = live_store
    assert process.stdout is not None and process.stdin is not None
    result: list[object] = []
    get_thread = threading.Thread(
        target=lambda: result.append(store.get("base", "__python_gil_delay__")), daemon=True
    )
    get_thread.start()
    assert _readline_with_timeout(process.stdout) == "DELAYED"
    close_thread = threading.Thread(target=store.close)
    close_thread.start()
    close_thread.join(timeout=2)
    assert not close_thread.is_alive()
    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.contains("base", "key")
    process.stdin.write("REPLY\n")
    process.stdin.flush()
    get_thread.join(timeout=5)
    assert not get_thread.is_alive()
    assert result == [7]
