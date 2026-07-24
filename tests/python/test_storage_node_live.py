"""Pure-Python process-isolated StorageNode integration coverage."""

from __future__ import annotations

import multiprocessing
import os
import time
from multiprocessing.connection import Connection

import pytest

import sitos


_DEADLINE = 20.0


def _node_worker(connection: Connection, prefix: str) -> None:
    try:
        with sitos.StorageNode(sitos.InMemoryEngine(), prefix=prefix) as node:
            views: dict[str, object] = {}
            connection.send(("READY", ""))
            while True:
                command, args = connection.recv()
                if command == "STOP":
                    connection.send(("OK", ""))
                    return
                if command == "CREATE":
                    node.create_session(args[0])
                    views[args[0]] = node.session_view(args[0])
                    connection.send(("OK", ""))
                elif command == "CLOSE":
                    node.close_session(args[0])
                    views.pop(args[0], None)
                    connection.send(("OK", ""))
                elif command == "GET":
                    try:
                        value = views[args[0]].get(args[1])
                    except sitos.NotFoundError:
                        connection.send(("MISSING", ""))
                    else:
                        connection.send(("VALUE", value))
                else:
                    raise RuntimeError(f"unknown node command: {command}")
    except BaseException as error:
        try:
            connection.send(("ERROR", repr(error)))
        except (BrokenPipeError, EOFError):
            pass


def _store_worker(connection: Connection, prefix: str) -> None:
    try:
        with sitos.ParamStore(prefix=prefix) as store:
            connection.send(("READY", ""))
            while True:
                command, args = connection.recv()
                if command == "STOP":
                    connection.send(("OK", ""))
                    return
                if command != "PUT":
                    raise RuntimeError(f"unknown store command: {command}")
                store.put(args[0], args[1], args[2])
                connection.send(("OK", ""))
    except BaseException as error:
        try:
            connection.send(("ERROR", repr(error)))
        except (BrokenPipeError, EOFError):
            pass


def _wait_ready(connection: Connection) -> None:
    deadline = time.monotonic() + _DEADLINE
    while time.monotonic() < deadline:
        if connection.poll(0.05):
            status, value = connection.recv()
            if status == "READY":
                return
            raise RuntimeError(value)
    raise TimeoutError("timed out waiting for worker readiness")


def _request(connection: Connection, command: str, *args: object) -> object:
    connection.send((command, args))
    deadline = time.monotonic() + _DEADLINE
    while time.monotonic() < deadline:
        if connection.poll(0.05):
            status, value = connection.recv()
            if status == "OK" or status == "READY":
                return value
            if status == "MISSING":
                return None
            if status == "VALUE":
                return value
            raise RuntimeError(value)
    raise TimeoutError(f"timed out waiting for {command}")


def _shutdown(process: multiprocessing.Process, connection: Connection | None) -> None:
    if connection is not None and process.is_alive():
        try:
            _request(connection, "STOP")
        except (BrokenPipeError, EOFError, TimeoutError, RuntimeError):
            pass
    process.join(timeout=5)
    if process.is_alive():
        process.terminate()
        process.join(timeout=5)
    if process.is_alive() and hasattr(process, "kill"):
        process.kill()
        process.join(timeout=5)
    assert not process.is_alive()


@pytest.mark.skipif(
    os.environ.get("SITOS_PYTHON_NODE_LIVE") != "1",
    reason="requires the serial Zenoh-enabled process-isolated lane",
)
def test_storage_node_python_process_topology_and_delivery() -> None:
    context = multiprocessing.get_context("spawn")
    prefix = f"sitos/python_node_{os.getpid()}"
    sid = "session_a"
    base_scope = "base"
    session_scope = f"session/{sid}"
    base_key = "base_snapshot_value"
    live_key = "live_delivery_value"
    base_value = os.getpid()
    live_value = os.getpid() + 1
    node_parent, node_child = context.Pipe()
    store_parent, store_child = context.Pipe()
    node_process = context.Process(target=_node_worker, args=(node_child, prefix))
    store_process = context.Process(target=_store_worker, args=(store_child, prefix))
    node_process.start()
    store_process.start()
    cache: sitos.ParamCache | None = None
    try:
        _wait_ready(node_parent)
        _wait_ready(store_parent)
        cache = sitos.ParamCache(prefix=prefix)

        session_created = False
        base_ready = False
        deadline = time.monotonic() + _DEADLINE
        while time.monotonic() < deadline and not base_ready:
            _request(store_parent, "PUT", base_scope, base_key, base_value)
            if session_created:
                _request(node_parent, "CLOSE", sid)
            _request(node_parent, "CREATE", sid)
            session_created = True
            base_ready = _request(node_parent, "GET", sid, base_key) == base_value
        assert base_ready, "base snapshot was not observed by SessionView"

        cache_ready = False
        deadline = time.monotonic() + _DEADLINE
        while time.monotonic() < deadline and not cache_ready:
            cache.detach()
            cache.attach(sid)
            try:
                cache_ready = cache.get(base_key) == base_value
            except sitos.NotFoundError:
                cache_ready = False
        assert cache_ready, "ParamCache did not observe the base snapshot"
        assert _request(node_parent, "GET", sid, live_key) is None
        assert cache.contains(live_key) is False

        node_seen = False
        cache_seen = False
        deadline = time.monotonic() + _DEADLINE
        while time.monotonic() < deadline and not (node_seen and cache_seen):
            _request(store_parent, "PUT", session_scope, live_key, live_value)
            node_seen = _request(node_parent, "GET", sid, live_key) == live_value
            cache_seen = cache.get(live_key, default=None) == live_value
        assert node_seen, "SessionView did not observe the live session value"
        assert cache_seen, "ParamCache did not observe the live session value"
    finally:
        if cache is not None:
            cache.close()
        _shutdown(node_process, node_parent)
        _shutdown(store_process, store_parent)
