"""Python subscription callback dispatch (Issue #26, docs/05 §2.1.1 and §3, requirement P04)."""

from __future__ import annotations

import dataclasses
import json
import logging
import os
import socket
import subprocess
import sys
import textwrap
import threading
import time
import uuid
from collections.abc import Callable, Iterator

import pytest

import sitos

_DEADLINE = 20.0


def _free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _node_config(port: int) -> str:
    return json.dumps(
        {
            "mode": "peer",
            "listen": {"endpoints": [f"tcp/127.0.0.1:{port}"]},
            "scouting": {"multicast": {"enabled": False}},
        }
    )


def _client_config(port: int) -> str:
    return json.dumps(
        {
            "mode": "client",
            "connect": {"endpoints": [f"tcp/127.0.0.1:{port}"]},
            "scouting": {"multicast": {"enabled": False}},
        }
    )


# docs/05 §2.4: independently opened sessions must run in separate processes with the pinned
# zenoh-c runtime, so the StorageNode runs in its own process and each test process opens at
# most one ParamStore at a time.
_NODE_SCRIPT = textwrap.dedent(
    """
    import sys

    import sitos

    prefix, node_config = sys.argv[1:3]
    with sitos.StorageNode(sitos.InMemoryEngine(), prefix=prefix, zenoh_config_json=node_config):
        print("READY", flush=True)
        sys.stdin.readline()
    """
)


def _readline(stream: object, timeout: float) -> str:
    lines: list[str] = []
    reader = threading.Thread(target=lambda: lines.append(stream.readline()), daemon=True)  # type: ignore[attr-defined]
    reader.start()
    reader.join(timeout)
    if not lines:
        raise AssertionError(f"node process produced no line within {timeout:g} seconds")
    return lines[0].strip()


@pytest.fixture(scope="module")
def node() -> Iterator[tuple[str, int]]:
    prefix = f"sitos/callbacks_{os.getpid()}_{uuid.uuid4().hex}"
    port = _free_port()
    process = subprocess.Popen(
        [sys.executable, "-c", _NODE_SCRIPT, prefix, _node_config(port)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
        env=os.environ.copy(),
    )
    try:
        assert _readline(process.stdout, _DEADLINE) == "READY"
        yield prefix, port
    finally:
        if process.stdin is not None:
            try:
                process.stdin.write("STOP\n")
                process.stdin.close()
            except OSError:
                pass
        try:
            process.wait(timeout=_DEADLINE)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=_DEADLINE)


@pytest.fixture
def store(node: tuple[str, int]) -> Iterator[sitos.ParamStore]:
    prefix, port = node
    with sitos.ParamStore(
        prefix=prefix, zenoh_config_json=_client_config(port), query_timeout_ms=2000
    ) as opened:
        deadline = time.monotonic() + _DEADLINE
        while True:
            try:
                opened.put("base", "ready", 1)
                break
            except sitos.SitosError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        yield opened


def _wait_until(predicate: Callable[[], bool], message: str) -> None:
    deadline = time.monotonic() + _DEADLINE
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError(message)
        time.sleep(0.01)


def _unique(name: str) -> str:
    return f"{name}/{uuid.uuid4().hex}/"


def test_public_subscription_surface() -> None:
    assert sitos.Subscription.__module__.startswith("sitos")
    change = sitos.ParamChange("put", "key", 1)
    assert (change.kind, change.key, change.value) == ("put", "key", 1)
    with pytest.raises(dataclasses.FrozenInstanceError):
        change.value = 2  # type: ignore[misc]
    assert "Subscription" in sitos.__all__
    assert "ParamChange" in sitos.__all__


def test_subscribe_validates_arguments(store: sitos.ParamStore) -> None:
    with pytest.raises(TypeError):
        store.subscribe("base", "x/", "not callable")  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        store.subscribe("snap/s1", "x/", lambda change: None)
    with pytest.raises(ValueError):
        store.subscribe("no-such-scope", "x/", lambda change: None)


def test_subscribe_on_closed_store_is_rejected(node: tuple[str, int]) -> None:
    prefix, port = node
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port))
    store.close()
    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.subscribe("base", "x/", lambda change: None)


class TestPythonCallbackDoesNotDeadlockWithGet:
    """Fixed docs/06 §5.1 name: callbacks read while the main side reads too (AC1)."""

    def test_repeated_rounds(self, store: sitos.ParamStore) -> None:
        for round_index in range(5):
            prefix = _unique(f"deadlock/{round_index}")
            expected = {f"{prefix}{index}": float(index) for index in range(20)}
            observed: dict[str, object] = {}
            lock = threading.Lock()

            def callback(change: sitos.ParamChange) -> None:
                value = store.get("base", change.key)
                with lock:
                    observed[change.key] = value

            stop_reader = threading.Event()
            reader_errors: list[BaseException] = []

            def reader() -> None:
                try:
                    while not stop_reader.is_set():
                        store.get("base", "ready")
                except BaseException as error:  # pragma: no cover - surfaced below
                    reader_errors.append(error)

            reader_thread = threading.Thread(target=reader, daemon=True)
            with store.subscribe("base", prefix, callback):
                reader_thread.start()
                try:
                    for key, value in expected.items():
                        store.put("base", key, value)
                        assert store.get("base", key) == value
                    _wait_until(
                        lambda: len(observed) == len(expected),
                        f"round {round_index}: callbacks stalled at {len(observed)}",
                    )
                finally:
                    stop_reader.set()
                    reader_thread.join(_DEADLINE)
            assert not reader_thread.is_alive()
            assert reader_errors == []
            assert observed == expected


def test_raising_callback_does_not_stop_notifications(
    store: sitos.ParamStore, caplog: pytest.LogCaptureFixture
) -> None:
    prefix = _unique("raising")
    received: list[str] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append(change.key)
        if len(received) == 1:
            raise RuntimeError("callback failure for the test")

    with caplog.at_level(logging.ERROR, logger="sitos"):
        with store.subscribe("base", prefix, callback):
            for index in range(3):
                store.put("base", f"{prefix}{index}", index)
            _wait_until(lambda: len(received) == 3, f"received only {received}")
    assert received == [f"{prefix}{index}" for index in range(3)]
    failures = [record for record in caplog.records if record.name == "sitos"]
    assert len(failures) == 1
    assert failures[0].levelno == logging.ERROR
    assert failures[0].exc_info is not None
    assert "callback failure for the test" in str(failures[0].exc_info[1])


def test_delivery_preserves_order_duplicates_batch_and_delete(
    store: sitos.ParamStore,
) -> None:
    prefix = _unique("ordered")
    received: list[tuple[str, str, object]] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append((change.kind, change.key, change.value))

    with store.subscribe("base", prefix, callback):
        store.put_batch(
            "base", [(f"{prefix}a", 1), (f"{prefix}b", "two"), (f"{prefix}a", 3.5)]
        )
        store.delete("base", f"{prefix}b")
        _wait_until(lambda: len(received) == 4, f"received only {received}")
    assert received == [
        ("put", f"{prefix}a", 1),
        ("put", f"{prefix}b", "two"),
        ("put", f"{prefix}a", 3.5),
        ("delete", f"{prefix}b", None),
    ]


def test_callbacks_run_on_a_daemon_dispatcher_thread(store: sitos.ParamStore) -> None:
    prefix = _unique("thread")
    threads: list[threading.Thread] = []

    with store.subscribe("base", prefix, lambda change: threads.append(threading.current_thread())):
        store.put("base", f"{prefix}x", 1)
        _wait_until(lambda: bool(threads), "no callback")
    assert threads[0] is not threading.main_thread()
    assert threads[0].daemon


def test_close_waits_for_in_flight_callback_and_discards_pending(
    store: sitos.ParamStore,
) -> None:
    prefix = _unique("close")
    started = threading.Event()
    release = threading.Event()
    received: list[str] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append(change.key)
        started.set()
        release.wait(_DEADLINE)

    subscription = store.subscribe("base", prefix, callback)
    sentinel: list[str] = []
    store.put("base", f"{prefix}first", 1)
    assert started.wait(_DEADLINE)
    with store.subscribe("base", prefix, lambda change: sentinel.append(change.key)):
        for index in range(3):
            store.put("base", f"{prefix}pending{index}", index)
        # A second subscription on the same session saw the pending changes, so they
        # reached the blocked subscription's queue before close() discards them.
        _wait_until(lambda: len(sentinel) == 3, f"sentinel saw {sentinel}")

    closer = threading.Thread(target=subscription.close)
    closer.start()
    closer.join(0.3)
    assert closer.is_alive(), "close returned while a callback was still running"
    release.set()
    closer.join(_DEADLINE)
    assert not closer.is_alive()

    store.put("base", f"{prefix}after", 1)
    time.sleep(0.3)
    assert received == [f"{prefix}first"]
    subscription.close()


def test_close_from_inside_its_own_callback_returns(store: sitos.ParamStore) -> None:
    prefix = _unique("self_close")
    received: list[str] = []
    closed = threading.Event()
    holder: list[sitos.Subscription] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append(change.key)
        holder[0].close()
        closed.set()

    holder.append(store.subscribe("base", prefix, callback))
    store.put("base", f"{prefix}a", 1)
    assert closed.wait(_DEADLINE), "close from inside the callback did not return"
    store.put("base", f"{prefix}b", 2)
    time.sleep(0.3)
    assert received == [f"{prefix}a"]
    holder[0].close()


def test_store_close_closes_its_subscriptions(node: tuple[str, int]) -> None:
    prefix, port = node
    key_prefix = _unique("store_close")
    received: list[str] = []
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port))
    subscription = store.subscribe("base", key_prefix, lambda change: received.append(change.key))
    dispatcher = subscription._thread
    store.close()
    assert not dispatcher.is_alive()
    subscription.close()

    control: list[str] = []
    # Opened only after the first store is closed: one session per process at a time.
    with sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port)) as other:
        with other.subscribe("base", key_prefix, lambda change: control.append(change.key)):
            other.put("base", f"{key_prefix}late", 1)
            _wait_until(lambda: control == [f"{key_prefix}late"], f"control saw {control}")
    assert received == []


@pytest.mark.filterwarnings("ignore::pytest.PytestUnhandledThreadExceptionWarning")
def test_base_exception_in_callback_stops_delivery(
    store: sitos.ParamStore, caplog: pytest.LogCaptureFixture
) -> None:
    prefix = _unique("system_exit")
    received: list[str] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append(change.key)
        raise SystemExit("leave the dispatcher")

    with caplog.at_level(logging.ERROR, logger="sitos"):
        subscription = store.subscribe("base", prefix, callback)
        store.put("base", f"{prefix}first", 1)
        _wait_until(lambda: not subscription._thread.is_alive(), "dispatcher kept running")
    records = [record for record in caplog.records if record.name == "sitos"]
    assert len(records) == 1
    assert records[0].exc_info is not None
    assert isinstance(records[0].exc_info[1], SystemExit)
    # The dispatcher closed native delivery on exit, so later changes are not queued unread.
    assert subscription._closed
    store.put("base", f"{prefix}later", 2)
    time.sleep(0.3)
    assert received == [f"{prefix}first"]
    subscription.close()


def test_concurrent_store_close_waits_for_the_first_close(node: tuple[str, int]) -> None:
    prefix, port = node
    key_prefix = _unique("concurrent_close")
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port))
    started = threading.Event()
    release = threading.Event()

    def slow(change: sitos.ParamChange) -> None:
        started.set()
        release.wait(_DEADLINE)

    store.subscribe("base", key_prefix, slow)
    store.put("base", f"{key_prefix}block", 1, ack=False)
    assert started.wait(_DEADLINE)

    first = threading.Thread(target=store.close)
    first.start()
    _wait_until(lambda: store._closing, "first close did not start")
    with pytest.raises(ValueError, match="ParamStore is closed"):
        store.subscribe("base", key_prefix, lambda change: None)
    second = threading.Thread(target=store.close)
    second.start()
    second.join(0.3)
    assert second.is_alive(), "a second close returned before the first finished"
    release.set()
    first.join(_DEADLINE)
    second.join(_DEADLINE)
    assert not first.is_alive() and not second.is_alive()


def test_store_close_from_two_callbacks_does_not_deadlock(node: tuple[str, int]) -> None:
    prefix, port = node
    key_prefix = _unique("callback_close")
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port))
    finished: list[str] = []
    barrier = threading.Barrier(2, timeout=_DEADLINE)

    def make_callback(name: str) -> Callable[[sitos.ParamChange], None]:
        def callback(change: sitos.ParamChange) -> None:
            barrier.wait()
            store.close()
            finished.append(name)

        return callback

    store.subscribe("base", f"{key_prefix}a/", make_callback("a"))
    store.subscribe("base", f"{key_prefix}b/", make_callback("b"))
    store.put("base", f"{key_prefix}a/x", 1, ack=False)
    store.put("base", f"{key_prefix}b/x", 1, ack=False)
    _wait_until(lambda: sorted(finished) == ["a", "b"], f"finished {finished}")
    store.close()


def test_slow_subscription_does_not_delay_another(store: sitos.ParamStore) -> None:
    slow_prefix = _unique("slow")
    fast_prefix = _unique("fast")
    release = threading.Event()
    fast: list[str] = []

    with store.subscribe("base", slow_prefix, lambda change: release.wait(_DEADLINE)):
        with store.subscribe("base", fast_prefix, lambda change: fast.append(change.key)):
            store.put("base", f"{slow_prefix}block", 1)
            for index in range(5):
                store.put("base", f"{fast_prefix}{index}", index)
            _wait_until(lambda: len(fast) == 5, f"fast subscription stalled at {fast}")
            release.set()


_SHUTDOWN_SCRIPT = textwrap.dedent(
    """
    import sys
    import threading
    import time

    import sitos

    client_config, prefix, keys, mode = sys.argv[1:5]
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=client_config, query_timeout_ms=2000)
    deadline = time.monotonic() + 20
    while True:
        try:
            store.put("base", "ready", 1)
            break
        except sitos.SitosError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
    entered = threading.Event()

    def keep_reading():
        stop = time.monotonic() + 0.5
        while time.monotonic() < stop:
            store.get("base", "ready")

    def callback(change):
        if not change.key.startswith(f"{keys}0/"):
            return
        entered.set()
        if mode == "in-flight-get":
            keep_reading()
        elif mode == "in-flight-sleep":
            time.sleep(0.5)
        elif mode == "self-close-then-get":
            subscriptions[0].close()
            keep_reading()
        elif mode == "store-close-then-sleep":
            store.close()
            time.sleep(0.5)
        elif mode == "subscribe-during-shutdown":
            time.sleep(0.5)
            try:
                store.subscribe("base", "late/", lambda late: None)
            except RuntimeError:
                print("RESUBSCRIBE_REJECTED", flush=True)
        if mode != "idle":
            # Printed only if interpreter exit waited for this in-flight callback.
            print("CALLBACK_DONE", flush=True)

    subscriptions = [store.subscribe("base", f"{keys}{index}/", callback) for index in range(3)]

    def put_all(suffix):
        try:
            for index in range(3):
                store.put("base", f"{keys}{index}/{suffix}", index, ack=False)
        except ValueError:
            pass  # store-close-then-sleep may already have closed the store

    put_all("key")
    if not entered.wait(20):
        raise SystemExit("callback never ran")
    put_all("again")
    print("EXITING", flush=True)
    """
)


@pytest.mark.parametrize(
    "mode",
    [
        "idle",
        "in-flight-sleep",
        "in-flight-get",
        "self-close-then-get",
        "store-close-then-sleep",
        "subscribe-during-shutdown",
    ],
)
def test_interpreter_shutdown_with_active_subscriptions(
    node: tuple[str, int], mode: str
) -> None:
    prefix, port = node
    completed = subprocess.run(
        [
            sys.executable,
            "-c",
            _SHUTDOWN_SCRIPT,
            _client_config(port),
            prefix,
            _unique(f"shutdown/{mode}"),
            mode,
        ],
        capture_output=True,
        text=True,
        timeout=60,
        env=os.environ.copy(),
    )
    assert completed.returncode == 0, completed.stderr
    expected = {"EXITING"}
    if mode != "idle":
        expected.add("CALLBACK_DONE")
    if mode == "subscribe-during-shutdown":
        expected.add("RESUBSCRIBE_REJECTED")
    lines = completed.stdout.split()
    assert sorted(lines) == sorted(expected), completed.stdout
    assert "Fatal Python error" not in completed.stderr
    assert "leaked" not in completed.stderr
