"""Python subscription callback dispatch (Issue #26, docs/05 §2.1 and §3, requirement P04)."""

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


@pytest.fixture(scope="module")
def node() -> Iterator[tuple[str, int]]:
    prefix = f"sitos/callbacks_{os.getpid()}_{uuid.uuid4().hex}"
    port = _free_port()
    with sitos.StorageNode(
        sitos.InMemoryEngine(), prefix=prefix, zenoh_config_json=_node_config(port)
    ):
        yield prefix, port


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


def test_python_callback_does_not_deadlock_with_get(store: sitos.ParamStore) -> None:
    """PythonCallbackDoesNotDeadlockWithGet: callbacks read while the main side reads too."""
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
    with sitos.ParamStore(prefix=prefix, zenoh_config_json=_client_config(port)) as other:
        with other.subscribe("base", key_prefix, lambda change: control.append(change.key)):
            other.put("base", f"{key_prefix}late", 1)
            _wait_until(lambda: control == [f"{key_prefix}late"], f"control saw {control}")
    assert received == []


@pytest.mark.filterwarnings("ignore::pytest.PytestUnhandledThreadExceptionWarning")
def test_base_exception_in_callback_stops_delivery(store: sitos.ParamStore) -> None:
    prefix = _unique("system_exit")
    received: list[str] = []

    def callback(change: sitos.ParamChange) -> None:
        received.append(change.key)
        raise SystemExit("leave the dispatcher")

    subscription = store.subscribe("base", prefix, callback)
    store.put("base", f"{prefix}first", 1)
    _wait_until(lambda: not subscription._thread.is_alive(), "dispatcher kept running")
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

    node_config, client_config, prefix, mode = sys.argv[1:5]
    node = sitos.StorageNode(sitos.InMemoryEngine(), prefix=prefix, zenoh_config_json=node_config)
    store = sitos.ParamStore(prefix=prefix, zenoh_config_json=client_config, query_timeout_ms=2000)
    deadline = time.monotonic() + 20
    while True:
        try:
            store.put("base", "ready", 1)
            break
        except sitos.SitosError:
            if time.monotonic() >= deadline:
                raise
    entered = threading.Event()

    other = sitos.ParamStore(prefix=prefix, zenoh_config_json=client_config, query_timeout_ms=2000)

    def keep_reading(reader):
        stop = time.monotonic() + 0.5
        while time.monotonic() < stop:
            reader.get("base", "ready")

    def callback(change):
        if change.key.startswith("shutdown/0/"):
            entered.set()
        else:
            return
        if mode == "in-flight-get":
            keep_reading(store)
        elif mode == "in-flight-sleep":
            time.sleep(0.5)
        elif mode == "self-close-then-get":
            subscriptions[0].close()
            keep_reading(store)
        elif mode == "store-close-then-get":
            store.close()
            keep_reading(other)
        elif mode == "subscribe-during-shutdown":
            time.sleep(0.5)
            try:
                store.subscribe("base", "late/", lambda late: None)
            except RuntimeError:
                print("RESUBSCRIBE_REJECTED", flush=True)

    subscriptions = [store.subscribe("base", f"shutdown/{index}/", callback) for index in range(3)]

    def put_all(suffix):
        try:
            for index in range(3):
                store.put("base", f"shutdown/{index}/{suffix}", index, ack=False)
        except ValueError:
            pass  # store-close-then-get may already have closed the store

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
        "store-close-then-get",
        "subscribe-during-shutdown",
    ],
)
def test_interpreter_shutdown_with_active_subscriptions(mode: str) -> None:
    port = _free_port()
    prefix = f"sitos/shutdown_{os.getpid()}_{uuid.uuid4().hex}"
    completed = subprocess.run(
        [
            sys.executable,
            "-c",
            _SHUTDOWN_SCRIPT,
            _node_config(port),
            _client_config(port),
            prefix,
            mode,
        ],
        capture_output=True,
        text=True,
        timeout=60,
        env=os.environ.copy(),
    )
    assert completed.returncode == 0, completed.stderr
    expected = ["EXITING"]
    if mode == "subscribe-during-shutdown":
        expected.append("RESUBSCRIBE_REJECTED")
    assert completed.stdout.split() == expected
    assert "Fatal Python error" not in completed.stderr
    assert "leaked" not in completed.stderr
