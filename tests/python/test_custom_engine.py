"""Python custom storage engines (Issue #28, docs/05 §2.4.1, requirements X01 and N03)."""

from __future__ import annotations

import gc
import json
import logging
import os
import socket
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import uuid
import weakref
from collections.abc import Callable, Iterable, Iterator

import pytest

import sitos

_DEADLINE = 20.0


class DictReader(sitos.StorageReader):
    def __init__(self, data: dict[str, bytes]) -> None:
        self._data = dict(data)

    def get(self, key: str) -> bytes | None:
        return self._data.get(key)

    def list(self, prefix: str) -> Iterable[tuple[str, bytes]]:
        # Deliberately unordered: the adapter owns the ordering contract.
        return [(key, value) for key, value in reversed(self._data.items()) if key.startswith(prefix)]


class DictEngine(sitos.StorageEngine):
    """A dict-based engine without take_snapshot (N03 copy fallback)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: dict[str, bytes] = {}

    def get(self, key: str) -> bytes | None:
        with self._lock:
            return self._data.get(key)

    def list(self, prefix: str) -> Iterable[tuple[str, bytes]]:
        with self._lock:
            return [
                (key, value)
                for key, value in reversed(self._data.items())
                if key.startswith(prefix)
            ]

    def put(self, key: str, value: bytes) -> None:
        with self._lock:
            self._data[key] = value

    def delete(self, key: str) -> None:
        with self._lock:
            self._data.pop(key, None)


class SnapshotDictEngine(DictEngine):
    def take_snapshot(self) -> sitos.StorageReader:
        with self._lock:
            return DictReader(self._data)


def _probe_or_skip(engine: object) -> object:
    probe_type = getattr(sitos._sitos, "_EngineProbe", None)
    if probe_type is None:
        pytest.skip("SITOS_PYTHON_TEST_SUPPORT is unavailable")
    return probe_type(engine)


ENGINES: dict[str, Callable[[], object]] = {
    "in_memory": sitos.InMemoryEngine,
    "python_fallback": DictEngine,
    "python_snapshot": SnapshotDictEngine,
}


@pytest.fixture(params=sorted(ENGINES))
def probe(request: pytest.FixtureRequest) -> object:
    """Drives the exact StorageEngine object StorageNode would use, from native code."""
    return _probe_or_skip(ENGINES[request.param]())


# --- Engine contract (port of tests/unit/storage_engine_contract.hpp) --------------------------


def test_get_returns_true_for_existing_key(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.get("a") == (True, [("a", b"1")])


def test_get_returns_true_even_if_sink_returns_false(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.get("a", sink_result=False) == (True, [("a", b"1")])


def test_get_returns_false_for_missing_key(probe) -> None:
    assert probe.get("missing") == (False, [])


def test_get_returns_false_for_deleted_key(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.delete("a")
    assert probe.get("a") == (False, [])


def test_put_overwrites_existing_key(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.put("a", b"2")
    assert probe.get("a") == (True, [("a", b"2")])


def test_put_empty_value(probe) -> None:
    assert probe.put("empty", b"")
    assert probe.get("empty") == (True, [("empty", b"")])


def test_delete_nonexistent_key_returns_true(probe) -> None:
    assert probe.delete("missing")


def test_list_enumerates_all_entries(probe) -> None:
    for key in ("b", "a", "c"):
        assert probe.put(key, key.encode())
    assert probe.list("") == (True, [("a", b"a"), ("b", b"b"), ("c", b"c")])


def test_list_with_prefix_filters_correctly(probe) -> None:
    for key in ("recon/fov", "recon/kernel", "reconx", "other"):
        assert probe.put(key, b"v")
    assert [key for key, _ in probe.list("recon/")[1]] == ["recon/fov", "recon/kernel"]
    assert [key for key, _ in probe.list("recon")[1]] == ["recon/fov", "recon/kernel", "reconx"]


def test_list_early_exit_returns_false(probe) -> None:
    for key in ("a", "b", "c"):
        assert probe.put(key, b"v")
    assert probe.list("", stop_after=1) == (False, [("a", b"v")])


def test_list_empty_engine(probe) -> None:
    assert probe.list("") == (True, [])


def test_list_emits_deterministic_key_order(probe) -> None:
    """ListEmitsDeterministicKeyOrder: ascending UTF-8 byte order regardless of engine order."""
    keys = ["z", "a/b", "a", "é", "B", "a/a"]
    for key in keys:
        assert probe.put(key, b"v")
    listed = [key for key, _ in probe.list("")[1]]
    assert listed == sorted(keys, key=lambda key: key.encode("utf-8"))


def test_snapshot_fallback_copies(probe) -> None:
    """SnapshotFallbackCopiesForInMemory, also for engines without take_snapshot (N03)."""
    assert probe.put("a", b"1")
    assert probe.put("b", b"2")
    snapshot = probe.snapshot()
    assert snapshot.list("") == (True, [("a", b"1"), ("b", b"2")])
    assert snapshot.get("a") == (True, [("a", b"1")])
    assert snapshot.get("missing") == (False, [])


def test_snapshot_is_isolated_from_base_put(probe) -> None:
    assert probe.put("a", b"old")
    snapshot = probe.snapshot()
    assert probe.put("a", b"new")
    assert probe.put("b", b"added")
    assert snapshot.get("a") == (True, [("a", b"old")])
    assert snapshot.get("b") == (False, [])


def test_snapshot_is_isolated_from_base_delete(probe) -> None:
    assert probe.put("a", b"1")
    snapshot = probe.snapshot()
    assert probe.delete("a")
    assert snapshot.get("a") == (True, [("a", b"1")])
    assert snapshot.list("") == (True, [("a", b"1")])


def test_handles_opaque_bytes(probe) -> None:
    values = {"nulls": b"\x00ab\x00cd\x00", "full": bytes(range(256)), "invalid_utf8": b"\xff"}
    for key, value in values.items():
        assert probe.put(key, value)
    for key, value in values.items():
        assert probe.get(key) == (True, [(key, value)])


def test_sink_can_reenter_read_operations(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.put("b", b"2")
    inner: list[object] = []
    found, _ = probe.get("a", action=lambda: inner.append(probe.list("")))
    assert found
    assert inner == [(True, [("a", b"1"), ("b", b"2")])]
    listed, _ = probe.list("", action=lambda: inner.append(probe.get("b")))
    assert listed
    assert inner[1:] == [(True, [("b", b"2")])] * 2


def test_sink_can_write_during_get(probe) -> None:
    assert probe.put("a", b"1")
    assert probe.get("a", action=lambda: probe.put("written", b"x"))[0]
    assert probe.get("written") == (True, [("written", b"x")])


def test_sink_can_write_during_list(probe) -> None:
    for key in ("a", "b"):
        assert probe.put(key, b"v")
    assert probe.list("", action=lambda: probe.delete("b"))[0]
    assert probe.get("b") == (False, [])


def test_python_engines_report_unsupported_sync(probe) -> None:
    capability, synced = probe.sync_capability(), probe.sync()
    if isinstance(probe.engine, sitos.InMemoryEngine):
        assert (capability, synced) == (1, True)
    else:
        assert (capability, synced) == (0, False)


# --- Python-adapter specifics (DEC-28-002, DEC-28-003, DEC-28-004) -----------------------------


class FailingEngine(DictEngine):
    def __init__(self) -> None:
        super().__init__()
        self.mode = ""

    def get(self, key: str) -> bytes | None:
        if self.mode == "raise":
            raise KeyError("engine get failure for the test")
        if self.mode == "not-bytes":
            return "text"  # type: ignore[return-value]
        return super().get(key)

    def list(self, prefix: str) -> Iterable[tuple[str, bytes]]:
        if self.mode == "raise":
            raise KeyError("engine list failure for the test")
        if self.mode == "bad-shape":
            return [("a", b"1", b"extra")]
        return super().list(prefix)

    def put(self, key: str, value: bytes) -> bool | None:
        if self.mode == "raise":
            raise KeyError("engine put failure for the test")
        if self.mode == "false":
            return False
        return super().put(key, value)

    def delete(self, key: str) -> bool | None:
        if self.mode == "raise":
            raise KeyError("engine delete failure for the test")
        if self.mode == "false":
            return False
        return super().delete(key)


@pytest.mark.parametrize("mode", ["raise", "not-bytes"])
def test_get_failures_surface_as_engine_failures(
    mode: str, caplog: pytest.LogCaptureFixture
) -> None:
    engine = FailingEngine()
    probe = _probe_or_skip(engine)
    engine.mode = mode
    with caplog.at_level(logging.ERROR, logger="sitos"):
        with pytest.raises(RuntimeError, match="engine failure"):
            probe.get("a")
    assert [record.name for record in caplog.records] == ["sitos"]


@pytest.mark.parametrize("mode", ["raise", "bad-shape"])
def test_list_failures_surface_as_engine_failures(
    mode: str, caplog: pytest.LogCaptureFixture
) -> None:
    engine = FailingEngine()
    probe = _probe_or_skip(engine)
    engine.mode = mode
    with caplog.at_level(logging.ERROR, logger="sitos"):
        with pytest.raises(RuntimeError, match="engine failure"):
            probe.list("a")
    assert [record.name for record in caplog.records] == ["sitos"]


@pytest.mark.parametrize("mode", ["raise", "false"])
def test_write_failures_return_false(mode: str, caplog: pytest.LogCaptureFixture) -> None:
    engine = FailingEngine()
    probe = _probe_or_skip(engine)
    engine.mode = mode
    with caplog.at_level(logging.ERROR, logger="sitos"):
        assert probe.put("a", b"1") is False
        assert probe.delete("a") is False
    expected = 2 if mode == "raise" else 0
    assert len([record for record in caplog.records if record.name == "sitos"]) == expected


def test_python_exception_is_logged_with_traceback(caplog: pytest.LogCaptureFixture) -> None:
    engine = FailingEngine()
    probe = _probe_or_skip(engine)
    engine.mode = "raise"
    with caplog.at_level(logging.ERROR, logger="sitos"):
        assert probe.put("a", b"1") is False
    (record,) = caplog.records
    assert record.exc_info is not None
    assert isinstance(record.exc_info[1], KeyError)


def test_fallback_snapshot_no_longer_calls_python() -> None:
    engine = FailingEngine()
    probe = _probe_or_skip(engine)
    assert probe.put("a", b"1")
    snapshot = probe.snapshot()
    engine.mode = "raise"
    assert snapshot.get("a") == (True, [("a", b"1")])


def test_take_snapshot_must_return_a_storage_reader(caplog: pytest.LogCaptureFixture) -> None:
    class BadSnapshotEngine(DictEngine):
        def take_snapshot(self) -> object:
            return {"a": b"1"}

    probe = _probe_or_skip(BadSnapshotEngine())
    with caplog.at_level(logging.ERROR, logger="sitos"):
        with pytest.raises(RuntimeError, match="engine failure"):
            probe.snapshot()
    assert [record.name for record in caplog.records] == ["sitos"]


def test_dropping_the_native_owner_releases_the_engine() -> None:
    engine = DictEngine()
    reference = weakref.ref(engine)
    probe = _probe_or_skip(engine)
    del engine
    assert reference() is not None
    probe.drop_without_gil()
    del probe
    gc.collect()
    assert reference() is None


def test_base_classes_are_abstract_and_documented() -> None:
    assert "GIL" in (sitos.StorageEngine.__doc__ or "")
    assert "C++" in (sitos.StorageEngine.__doc__ or "")
    with pytest.raises(NotImplementedError):
        sitos.StorageEngine().put("a", b"1")
    with pytest.raises(NotImplementedError):
        sitos.StorageReader().get("a")


def test_storage_node_rejects_other_engine_types() -> None:
    with pytest.raises(TypeError):
        sitos.StorageNode(object())  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        sitos.StorageNode({"a": b"1"})  # type: ignore[arg-type]


# --- End to end through a StorageNode process (AC2, AC3) -----------------------------------------


def _free_port() -> int:
    with socket.socket() as probe_socket:
        probe_socket.bind(("127.0.0.1", 0))
        return probe_socket.getsockname()[1]


def _config(mode: str, port: int) -> str:
    if mode == "peer":
        return json.dumps(
            {
                "mode": "peer",
                "listen": {"endpoints": [f"tcp/127.0.0.1:{port}"]},
                "scouting": {"multicast": {"enabled": False}},
            }
        )
    return json.dumps(
        {
            "mode": "client",
            "connect": {"endpoints": [f"tcp/127.0.0.1:{port}"]},
            "scouting": {"multicast": {"enabled": False}},
        }
    )


# docs/05 §2.4: independently opened sessions run in separate processes with the pinned
# zenoh-c runtime, so the node owning the Python engine runs in its own process.
_NODE_SCRIPT = textwrap.dedent(
    """
    import sys
    import threading
    import time

    import sitos

    prefix, node_config, mode = sys.argv[1:4]


    class DictEngine(sitos.StorageEngine):
        def __init__(self):
            self._lock = threading.Lock()
            self._data = {}

        def get(self, key):
            if key.startswith("fail/"):
                raise KeyError("engine get failure for the test")
            with self._lock:
                return self._data.get(key)

        def list(self, prefix):
            with self._lock:
                return [(k, v) for k, v in self._data.items() if k.startswith(prefix)]

        def put(self, key, value):
            if key.startswith("fail/"):
                return False
            if mode == "slow-put" and key.startswith("slow/"):
                print("PUT_STARTED", flush=True)
                time.sleep(0.5)
                print("PUT_DONE", flush=True)
            with self._lock:
                self._data[key] = value

        def delete(self, key):
            with self._lock:
                self._data.pop(key, None)


    class DictReader(sitos.StorageReader):
        def __init__(self, data):
            self._data = data

        def get(self, key):
            return self._data.get(key)

        def list(self, prefix):
            return [(k, v) for k, v in self._data.items() if k.startswith(prefix)]


    class SnapshotEngine(DictEngine):
        def take_snapshot(self):
            with self._lock:
                return DictReader(dict(self._data))


    engine = SnapshotEngine() if mode == "snapshot" else DictEngine()
    node = sitos.StorageNode(engine, prefix=prefix, zenoh_config_json=node_config)
    stop_stress = threading.Event()
    stress_rounds = []

    def stress():
        # Python-thread node operations interleaved with zenoh-thread engine calls.
        try:
            while not stop_stress.is_set():
                node.active_sessions()
                node.create_session("stress")
                node.session_view("stress").items("")
                node.close_session("stress")
                stress_rounds.append(1)
        except BaseException as error:
            print(f"STRESS_FAILED {type(error).__name__}: {error}", flush=True)

    stress_thread = threading.Thread(target=stress, daemon=True)
    if mode == "stress":
        stress_thread.start()
    print("READY", flush=True)
    for line in sys.stdin:
        command, _, argument = line.strip().partition(" ")
        if command == "CREATE":
            node.create_session(argument)
            print("CREATED", flush=True)
        elif command == "EXIT":
            # Exit without stopping the node; the atexit handler must stop it.
            print("EXITING", flush=True)
            break
        elif command == "ITEMS":
            print("ITEMS " + repr(list(node.session_view(argument).items(""))), flush=True)
        elif command == "STOP":
            stop_stress.set()
            if mode == "stress":
                stress_thread.join(20)
                print(f"STRESS_ROUNDS {len(stress_rounds)}", flush=True)
            node.stop()
            print("STOPPED", flush=True)
            break
    """
)


class NodeProcess:
    def __init__(self, mode: str) -> None:
        self.prefix = f"sitos/custom_engine_{os.getpid()}_{uuid.uuid4().hex}"
        failures: list[str] = []
        for _ in range(3):
            self.port = _free_port()
            self._stderr = tempfile.TemporaryFile("w+")
            self.process = subprocess.Popen(
                [sys.executable, "-c", _NODE_SCRIPT, self.prefix, _config("peer", self.port), mode],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self._stderr,
                text=True,
                env=os.environ.copy(),
            )
            if self.readline() == "READY":
                return
            self.process.kill()
            self.process.wait(timeout=_DEADLINE)
            failures.append(f"rc={self.process.returncode} stderr={self.stderr()[-2000:]!r}")
            self._stderr.close()
        raise AssertionError(f"StorageNode process never became ready: {failures}")

    def readline(self) -> str:
        lines: list[str] = []
        reader = threading.Thread(
            target=lambda: lines.append(self.process.stdout.readline()),  # type: ignore[union-attr]
            daemon=True,
        )
        reader.start()
        reader.join(_DEADLINE)
        return lines[0].strip() if lines else ""

    def command(self, line: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stderr(self) -> str:
        self._stderr.seek(0)
        return self._stderr.read()

    def finish(self) -> tuple[int, str, str]:
        out, _ = self.process.communicate(timeout=60)
        result = (self.process.returncode, out, self.stderr())
        self._stderr.close()
        return result


@pytest.fixture(params=["plain", "snapshot"])
def node_store(request: pytest.FixtureRequest) -> Iterator[tuple[NodeProcess, sitos.ParamStore]]:
    node = NodeProcess(request.param)
    try:
        with sitos.ParamStore(
            prefix=node.prefix,
            zenoh_config_json=_config("client", node.port),
            query_timeout_ms=2000,
        ) as store:
            _wait_ready(store)
            yield node, store
    finally:
        if node.process.poll() is None:
            node.command("STOP")
            node.finish()


def _wait_ready(store: sitos.ParamStore) -> None:
    deadline = time.monotonic() + _DEADLINE
    while True:
        try:
            store.put("base", "ready", 1)
            return
        except sitos.SitosError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def test_storage_node_serves_puts_and_gets_from_a_python_engine(node_store) -> None:
    """AC3: a StorageNode on a pure-Python engine serves gets and puts end to end."""
    _, store = node_store
    store.put("base", "recon/fov", 240.0)
    store.put_batch("base", [("recon/kernel", "sharp"), ("recon/count", 3), ("recon/count", 4)])
    assert store.get("base", "recon/fov") == 240.0
    assert store.get("base", "recon/count") == 4
    assert list(store.list("base", "recon/")) == [
        ("recon/count", 4),
        ("recon/fov", 240.0),
        ("recon/kernel", "sharp"),
    ]
    store.delete("base", "recon/kernel")
    deadline = time.monotonic() + _DEADLINE
    while store.contains("base", "recon/kernel"):
        assert time.monotonic() < deadline
        time.sleep(0.01)


def test_python_engine_failures_reach_clients_as_engine_failures(node_store) -> None:
    """DEC-28-002: failed writes report OutcomeUnknown; a raising read produces no reply."""
    _, store = node_store
    with pytest.raises(sitos.OutcomeUnknownError):
        store.put("base", "fail/write", 1)
    with pytest.raises(sitos.NotFoundError):
        store.get("base", "fail/read")
    store.put("base", "ok/after", 2)
    assert store.get("base", "ok/after") == 2


def test_snapshot_isolation_end_to_end(node_store) -> None:
    """AC2: session snapshots ignore later base writes, through the N03 fallback ("plain") and
    through a Python take_snapshot reader ("snapshot"), over zenoh and through SessionView."""
    node, store = node_store
    store.put("base", "iso/value", 1)
    store.put("base", "iso/deleted", 2)
    sid = f"s{uuid.uuid4().hex}"
    node.command(f"CREATE {sid}")
    assert node.readline() == "CREATED"
    store.put("base", "iso/value", 99)
    store.put("base", "iso/added", 3)
    store.delete("base", "iso/deleted")
    snap = f"snap/{sid}"
    assert store.get(snap, "iso/value") == 1
    assert store.get(snap, "iso/deleted") == 2
    assert store.get(snap, "iso/added", default=None) is None
    assert store.get("base", "iso/value") == 99
    node.command(f"ITEMS {sid}")
    assert node.readline() == "ITEMS " + repr([("iso/deleted", 2), ("iso/value", 1), ("ready", 1)])


def test_python_thread_node_calls_do_not_deadlock_with_engine_calls() -> None:
    node = NodeProcess("stress")
    with sitos.ParamStore(
        prefix=node.prefix, zenoh_config_json=_config("client", node.port), query_timeout_ms=2000
    ) as store:
        _wait_ready(store)
        for index in range(200):
            store.put("base", f"stress/{index}", index)
        assert len(list(store.list("base", "stress/"))) == 200
    node.command("STOP")
    rc, out, err = node.finish()
    assert rc == 0, err
    lines = out.splitlines()
    assert lines[-1] == "STOPPED", (out, err)
    assert len(lines) == 2 and lines[0].startswith("STRESS_ROUNDS "), (out, err)
    assert int(lines[0].split()[1]) > 0, (out, err)


def test_interpreter_exit_stops_a_node_with_an_in_flight_python_engine_call() -> None:
    """Exit while a zenoh thread is inside the Python engine: atexit stops the node first."""
    node = NodeProcess("slow-put")
    with sitos.ParamStore(
        prefix=node.prefix, zenoh_config_json=_config("client", node.port), query_timeout_ms=2000
    ) as store:
        _wait_ready(store)
        store.put("base", "slow/key", 1, ack=False)
        assert node.readline() == "PUT_STARTED"
        node.command("EXIT")
        rc, out, err = node.finish()
    assert rc == 0, err
    # PUT_DONE proves exit waited for the in-flight engine call before finalization.
    assert sorted(out.split()) == ["EXITING", "PUT_DONE"], (out, err)
    assert "Fatal Python error" not in err
    assert "leaked" not in err
