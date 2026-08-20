#!/usr/bin/env python3
"""Bounded acceptance driver for the source-only Python examples."""
from __future__ import annotations

import argparse
import ast
import contextlib
import ctypes
import importlib.util
import io
import multiprocessing
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
import uuid
import zipfile
from ctypes import wintypes
from multiprocessing.connection import Connection
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples" / "python"
REQUIRED = {
    "quickstart": EXAMPLES / "quickstart.py",
    "numpy-lut": EXAMPLES / "numpy_lut.py",
    "raw-zenoh": EXAMPLES / "raw_zenoh.py",
}
MARKERS = {
    "quickstart": "PYTHON_QUICKSTART_OK",
    "numpy-lut": "PYTHON_NUMPY_LUT_OK",
    "raw-zenoh": "PYTHON_RAW_ZENOH_OK",
}
CASE_SECONDS = 60.0


class _FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += max(0.0, seconds)


class _FakeProcess:
    def __init__(
        self,
        clock: _FakeClock,
        *,
        alive: bool = True,
        exitcode: int = 0,
        natural_exit_at: float | None = None,
        terminate_exits: bool = True,
        kill_exits: bool = True,
        terminate_error: BaseException | None = None,
        kill_error: BaseException | None = None,
    ) -> None:
        self.clock = clock
        self._alive = alive
        self._exitcode = None if alive else exitcode
        self.natural_exit_at = natural_exit_at
        self.natural_exitcode = exitcode
        self.terminate_exits = terminate_exits
        self.kill_exits = kill_exits
        self.terminate_error = terminate_error
        self.kill_error = kill_error
        self.terminate_calls = 0
        self.kill_calls = 0
        self.join_calls: list[float] = []

    def _refresh(self) -> None:
        if (
            self._alive
            and self.natural_exit_at is not None
            and self.clock.now >= self.natural_exit_at
        ):
            self._alive = False
            self._exitcode = self.natural_exitcode

    @property
    def exitcode(self) -> int | None:
        self._refresh()
        return self._exitcode

    def is_alive(self) -> bool:
        self._refresh()
        return self._alive

    def join(self, timeout: float) -> None:
        self.join_calls.append(timeout)
        self.clock.advance(timeout)
        self._refresh()

    def terminate(self) -> None:
        self.terminate_calls += 1
        if self.terminate_error is not None:
            raise self.terminate_error
        if self.terminate_exits:
            self._alive = False
            self._exitcode = -15

    def kill(self) -> None:
        self.kill_calls += 1
        if self.kill_error is not None:
            raise self.kill_error
        if self.kill_exits:
            self._alive = False
            self._exitcode = -9


class _FakeConnection:
    def __init__(
        self,
        clock: _FakeClock,
        *,
        packet_at: float | None = None,
        packet: tuple[str, object] = ("OK", ""),
        send_error: BaseException | None = None,
        poll_overshoot: float = 0.0,
    ) -> None:
        self.clock = clock
        self.packet_at = packet_at
        self.packet = packet
        self.send_error = send_error
        self.poll_overshoot = poll_overshoot
        self.sent: list[object] = []
        self.send_times: list[float] = []
        self.closed = False
        self.received = False
        self.recv_times: list[float] = []

    def send(self, value: object) -> None:
        if self.send_error is not None:
            raise self.send_error
        self.sent.append(value)
        self.send_times.append(self.clock.now)

    def poll(self, wait: float = 0.0) -> bool:
        actual_wait = wait + self.poll_overshoot
        self.poll_overshoot = 0.0
        if self.packet_at is not None and not self.received:
            if self.packet_at <= self.clock.now + actual_wait:
                self.clock.advance(self.packet_at - self.clock.now)
                return True
        self.clock.advance(actual_wait)
        return False

    def recv(self) -> tuple[str, object]:
        self.received = True
        self.recv_times.append(self.clock.now)
        return self.packet

    def close(self) -> None:
        self.closed = True


def _load_quickstart() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location(
        "sitos_example_quickstart_contract", REQUIRED["quickstart"]
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _import_roots(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    roots: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            roots.update(alias.name.partition(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            roots.add(node.module.partition(".")[0])
    return roots


def _pid_alive(pid: int) -> bool:
    """Treat unknown or access-denied process state as alive."""
    if pid <= 0:
        return True
    if os.name != "nt":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except (PermissionError, OSError):
            return True
        return True
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return ctypes.get_last_error() != 87  # ERROR_INVALID_PARAMETER means dead.
    try:
        return kernel32.WaitForSingleObject(handle, 0) != 0
    finally:
        kernel32.CloseHandle(handle)


def _remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("acceptance case deadline expired")
    return remaining


def _bounded_communicate(
    process: subprocess.Popen[str], deadline: float
) -> tuple[str, str]:
    return process.communicate(timeout=_remaining(deadline))


def _kill_tree(process: subprocess.Popen[str], deadline: float) -> str | None:
    if os.name == "nt":
        try:
            killer = subprocess.Popen(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError as error:
            if process.poll() is None:
                process.kill()
            return f"could not start taskkill: {error}"
        try:
            _, killer_stderr = _bounded_communicate(killer, deadline)
        except (subprocess.TimeoutExpired, TimeoutError):
            killer.kill()
            try:
                _bounded_communicate(killer, deadline)
            except (subprocess.TimeoutExpired, TimeoutError):
                pass
            if process.poll() is None:
                process.kill()
            return "taskkill did not finish before the case deadline"
        if killer.returncode != 0:
            if process.poll() is None:
                process.kill()
            return f"taskkill failed ({killer.returncode}): {killer_stderr.strip()}"
        return None

    # The process is a process-group leader. Signal the group even when the
    # leader has exited because a descendant may still own captured pipe handles.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return None
    except OSError as error:
        if process.poll() is None:
            process.kill()
        return f"SIGTERM process-group cleanup failed: {error}"
    try:
        process.wait(timeout=min(1.0, _remaining(deadline)))
    except (subprocess.TimeoutExpired, TimeoutError):
        pass
    try:
        os.killpg(process.pid, 0)
    except ProcessLookupError:
        return None
    except OSError as error:
        return f"process-group liveness check failed: {error}"
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return None
    except OSError as error:
        return f"SIGKILL process-group cleanup failed: {error}"
    return None


def _run_process(
    command: list[str],
    *,
    cwd: str,
    env: dict[str, str],
    deadline: float,
) -> subprocess.CompletedProcess[str]:
    kwargs: dict[str, object] = {
        "cwd": cwd,
        "env": env,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "text": True,
    }
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    # Refuse to start a coordinator without five seconds left for terminating
    # and reaping its process tree if the coordinator stalls.
    remaining = _remaining(deadline)
    if remaining <= 5.0:
        return subprocess.CompletedProcess(
            command, 124, "", "insufficient time remains in the acceptance case"
        )
    run_budget = remaining - 5.0
    process = subprocess.Popen(command, **kwargs)
    try:
        stdout, stderr = process.communicate(timeout=run_budget)
    except subprocess.TimeoutExpired as error:
        cleanup_error = _kill_tree(process, deadline)
        try:
            stdout, stderr = _bounded_communicate(process, deadline)
        except (subprocess.TimeoutExpired, TimeoutError):
            if process.poll() is None:
                process.kill()
            for stream in (process.stdout, process.stderr):
                if stream is not None:
                    stream.close()
            stdout = error.stdout or ""
            stderr = error.stderr or ""
            cleanup_error = cleanup_error or "process tree did not close before deadline"
        if cleanup_error:
            stderr = f"{stderr}\nprocess cleanup failed: {cleanup_error}".lstrip()
        return subprocess.CompletedProcess(command, 124, stdout, stderr)
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def _env(environment: dict[str, str] | None = None) -> dict[str, str]:
    result = os.environ.copy()
    if result.get("SITOS_EXAMPLE_KEEP_PYTHONPATH") != "1":
        result.pop("PYTHONPATH", None)
    if environment:
        result.update(environment)
    return result


def _public_peer(connection: Connection, prefix: str) -> None:
    node = None
    try:
        import sitos

        node = sitos.StorageNode(
            sitos.InMemoryEngine(), prefix=prefix, zenoh_config_json=None
        )
        connection.send(("READY", ""))
        command = connection.recv()
        if command != "STOP":
            raise RuntimeError(f"unknown peer command: {command}")
        node.stop()
        node = None
        connection.send(("OK", ""))
    except BaseException as error:
        try:
            connection.send(("ERROR", f"{type(error).__name__}: {error}"))
        except (BrokenPipeError, EOFError, OSError):
            pass
    finally:
        try:
            if node is not None:
                node.stop()
        finally:
            connection.close()


def _wait_pipe(
    connection: Connection,
    process: multiprocessing.Process,
    deadline: float,
) -> tuple[str, str]:
    while time.monotonic() < deadline:
        if connection.poll(min(0.05, max(0.0, deadline - time.monotonic()))):
            status, value = connection.recv()
            if status in {"READY", "OK"}:
                return status, value
            raise RuntimeError(value)
        if not process.is_alive() and not connection.poll():
            raise RuntimeError(f"peer exited with {process.exitcode}")
    raise TimeoutError("timed out waiting for peer")


def _join_peer_until(process: multiprocessing.Process, deadline: float) -> None:
    while process.is_alive() and time.monotonic() < deadline:
        process.join(min(0.2, max(0.0, deadline - time.monotonic())))


def _stop_peer(
    process: multiprocessing.Process, connection: Connection, deadline: float
) -> None:
    stop_error: BaseException | None = None
    try:
        if not process.is_alive():
            stop_error = AssertionError(
                f"peer exited before STOP acknowledgement: {process.exitcode}"
            )
        else:
            try:
                connection.send("STOP")
                status, value = _wait_pipe(
                    connection, process, min(deadline, time.monotonic() + 1.0)
                )
                if status != "OK" or value != "":
                    raise AssertionError(
                        f"invalid peer STOP response: {status}, {value}"
                    )
            except BaseException as error:
                stop_error = error
        _join_peer_until(process, min(deadline, time.monotonic() + 1.0))
        if process.is_alive():
            process.terminate()
            _join_peer_until(process, min(deadline, time.monotonic() + 1.0))
        if process.is_alive() and hasattr(process, "kill"):
            process.kill()
            _join_peer_until(process, deadline)
        if process.is_alive():
            stop_error = stop_error or AssertionError("public peer survived cleanup")
        elif process.exitcode != 0:
            stop_error = stop_error or AssertionError(
                f"public peer failed cleanup: {process.exitcode}"
            )
        if stop_error is not None:
            raise stop_error
    finally:
        connection.close()


def _run_script(
    name: str,
    *,
    deadline: float,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    prefix = f"sitos/example_{os.getpid()}_{uuid.uuid4().hex}"
    command = [sys.executable, str(REQUIRED[name])]
    if name == "raw-zenoh":
        command.extend(("--prefix", prefix))
    with tempfile.TemporaryDirectory(prefix="sitos-python-example-") as directory:
        return _run_process(command, cwd=directory, env=_env(environment), deadline=deadline)


def _raise_raw_failures(
    primary: BaseException | None, cleanup_error: BaseException | None
) -> None:
    if primary is not None:
        if cleanup_error is not None:
            raise RuntimeError(f"{primary}; raw peer cleanup: {cleanup_error}") from primary
        raise primary
    if cleanup_error is not None:
        raise cleanup_error


def _run_raw_with_peer(deadline: float) -> subprocess.CompletedProcess[str]:
    context = multiprocessing.get_context("spawn")
    prefix = f"sitos/example_peer_{os.getpid()}_{uuid.uuid4().hex}"
    parent, child = context.Pipe()
    peer = context.Process(target=_public_peer, args=(child, prefix), name="sitos-example-raw-peer")
    try:
        peer.start()
    except BaseException:
        parent.close()
        child.close()
        raise
    child.close()
    result: subprocess.CompletedProcess[str] | None = None
    primary: BaseException | None = None
    try:
        status, value = _wait_pipe(parent, peer, deadline - 5.0)
        if status != "READY" or value != "":
            raise RuntimeError(f"invalid peer READY response: {status}, {value}")
        with tempfile.TemporaryDirectory(prefix="sitos-python-raw-") as directory:
            command = [sys.executable, str(REQUIRED["raw-zenoh"]), "--prefix", prefix]
            result = _run_process(command, cwd=directory, env=_env(), deadline=deadline)
            if result.returncode != 0:
                primary = RuntimeError(
                    f"raw example exited {result.returncode}: {result.stderr.strip()}"
                )
    except BaseException as error:
        primary = error
    cleanup_error: BaseException | None = None
    try:
        _stop_peer(peer, parent, deadline)
    except BaseException as error:
        cleanup_error = error
    _raise_raw_failures(primary, cleanup_error)
    assert result is not None
    return result


class PythonExampleContractTest(unittest.TestCase):
    def test_required_example_files_exist(self) -> None:
        missing = [name for name, path in REQUIRED.items() if not path.is_file()]
        self.assertFalse(missing, f"required example scripts are absent: {missing}")

    def test_required_success_markers_are_declared(self) -> None:
        for name, marker in MARKERS.items():
            self.assertTrue(REQUIRED[name].is_file(), f"{name}: missing script")
            self.assertIn(marker, REQUIRED[name].read_text(encoding="utf-8"))

    def test_process_examples_declare_lifecycle_contract(self) -> None:
        quickstart = REQUIRED["quickstart"].read_text(encoding="utf-8")
        tree = ast.parse(quickstart, filename=str(REQUIRED["quickstart"]))
        calls = {
            node.func.attr
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
        }
        self.assertTrue(
            {"get_context", "create_session", "close_session", "detach", "close", "stop"}
            <= calls
        )
        self.assertIn('get_context("spawn")', quickstart)
        numpy = REQUIRED["numpy-lut"].read_text(encoding="utf-8")
        self.assertIn("import quickstart as _quickstart", numpy)
        self.assertIn("_quickstart.run_example", numpy)
        self.assertNotIn("def run(", numpy)
        self.assertIn("cache.detach()", numpy)
        self.assertIn("FAILURE_ENV", numpy)
        self.assertIn("FAILURE_VALUE", numpy)
        self.assertGreater(quickstart.rfind('marker="PYTHON_QUICKSTART_OK"'), quickstart.rfind("_cleanup"))
        self.assertGreater(numpy.rfind('marker="PYTHON_NUMPY_LUT_OK"'), numpy.rfind("_cleanup"))
        self.assertIn("SITOS_EXAMPLE_TEST_FAIL", quickstart)
        self.assertIn("SITOS_EXAMPLE_TEST_CHILD", quickstart)
        self.assertIn("FAILURE_CLEANUP_LINE", quickstart)
        self.assertIn("zenoh_config_json=None", quickstart)
        self.assertIn("FAILURE_ENV", numpy)
        self.assertIn("FAILURE_VALUE", numpy)
        self.assertEqual(
            _import_roots(REQUIRED["quickstart"]),
            {
                "__future__",
                "multiprocessing",
                "os",
                "sitos",
                "sys",
                "time",
                "typing",
                "uuid",
            },
        )
        self.assertEqual(
            _import_roots(REQUIRED["numpy-lut"]),
            {
                "__future__",
                "multiprocessing",
                "numpy",
                "os",
                "quickstart",
                "sitos",
                "sys",
                "uuid",
            },
        )

    def test_near_expiry_does_not_spawn_a_coordinator(self) -> None:
        with mock.patch("subprocess.Popen") as popen:
            result = _run_process(
                [sys.executable, "-c", "pass"],
                cwd=str(ROOT),
                env=os.environ.copy(),
                deadline=time.monotonic() + 0.5,
            )
        self.assertEqual(result.returncode, 124)
        popen.assert_not_called()

    def test_raw_primary_and_cleanup_failures_are_both_preserved(self) -> None:
        with self.assertRaisesRegex(
            RuntimeError, r"raw failed; raw peer cleanup: cleanup failed"
        ):
            _raise_raw_failures(RuntimeError("raw failed"), OSError("cleanup failed"))

    def test_pipe_failures_keep_stage_and_exit_diagnostics(self) -> None:
        examples_path = str(EXAMPLES)
        sys.path.insert(0, examples_path)
        try:
            import quickstart
        finally:
            sys.path.remove(examples_path)

        class DeadProcess:
            exitcode = 9

            def is_alive(self) -> bool:
                return False

        class ReceiveFailure:
            def poll(self, _wait: float = 0.0) -> bool:
                return True

            def recv(self) -> object:
                raise EOFError

        class SendFailure:
            def send(self, _value: object) -> None:
                raise BrokenPipeError

        process = DeadProcess()
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"cache attach: pipe receive failed; exit 9"
        ):
            quickstart._wait_packet(
                ReceiveFailure(), process, "cache attach", time.monotonic() + 1.0
            )
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"writer put: pipe send failed; exit 9"
        ):
            quickstart._request(
                SendFailure(), process, "writer put", time.monotonic() + 1.0, "PUT"
            )

    def test_cleanup_requests_all_stops_before_shared_deadline_wait(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        writer = _FakeProcess(clock, natural_exit_at=5.8)
        writer_connection = _FakeConnection(clock, packet_at=5.7)
        cache = _FakeProcess(clock, natural_exit_at=0.2)
        cache_connection = _FakeConnection(clock, packet_at=0.1)

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            failures = quickstart._cleanup(
                [
                    ("writer", writer, writer_connection),
                    ("cache", cache, cache_connection),
                ],
                deadline=19.0,
            )

        self.assertEqual(failures, [])
        self.assertEqual(writer_connection.send_times, [0.0])
        self.assertEqual(cache_connection.send_times, [0.0])
        for process, connection in (
            (writer, writer_connection),
            (cache, cache_connection),
        ):
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode, 0)
            self.assertEqual(process.terminate_calls, 0)
            self.assertEqual(process.kill_calls, 0)
            self.assertEqual(connection.sent, [("STOP", ())])
            self.assertTrue(connection.closed)

    def test_cleanup_reports_exit_between_liveness_check_and_stop(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()

        class ExitBetweenChecksProcess(_FakeProcess):
            def __init__(self) -> None:
                super().__init__(clock)
                self.is_alive_calls = 0

            def is_alive(self) -> bool:
                self.is_alive_calls += 1
                if self.is_alive_calls == 1:
                    return True
                self._alive = False
                self._exitcode = 0
                return False

        process = ExitBetweenChecksProcess()
        connection = _FakeConnection(clock)

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            failures = quickstart._cleanup(
                [("writer", process, connection)],
                deadline=2.0,
            )

        self.assertEqual(len(failures), 1)
        self.assertRegex(failures[0], r"cleanup writer:.*exited with 0")
        self.assertEqual(connection.sent, [("STOP", ())])
        self.assertTrue(connection.closed)

    def test_cleanup_rejects_ack_observed_after_shared_deadline(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        process = _FakeProcess(clock, natural_exit_at=1.01)
        connection = _FakeConnection(
            clock,
            packet_at=1.01,
            poll_overshoot=0.96,
        )

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            failures = quickstart._cleanup(
                [("writer", process, connection)],
                deadline=2.0,
            )

        self.assertEqual(len(failures), 1)
        self.assertRegex(failures[0], r"cleanup writer:.*timed out waiting")
        self.assertTrue(connection.received)
        self.assertAlmostEqual(clock.now, 1.01)
        self.assertFalse(process.is_alive())
        self.assertEqual(process.terminate_calls, 0)
        self.assertEqual(process.kill_calls, 0)
        self.assertTrue(connection.closed)

    def test_cleanup_escalates_and_reaps_all_survivors_by_phase(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        events: list[str] = []

        class DeferredKillProcess(_FakeProcess):
            def __init__(self, label: str) -> None:
                super().__init__(
                    clock,
                    terminate_exits=False,
                    kill_exits=False,
                )
                self.label = label
                self.kill_requested = False

            def terminate(self) -> None:
                self.terminate_calls += 1
                events.append(f"terminate:{self.label}")

            def kill(self) -> None:
                self.kill_calls += 1
                self.kill_requested = True
                events.append(f"kill:{self.label}")

            def join(self, timeout: float) -> None:
                self.join_calls.append(timeout)
                phase = "kill" if self.kill_requested else "terminate"
                events.append(f"{phase}-join:{self.label}")
                if self.kill_requested:
                    self._alive = False
                    self._exitcode = -9
                    clock.advance(min(timeout, 0.01))
                else:
                    clock.advance(timeout)

        writer = DeferredKillProcess("writer")
        cache = DeferredKillProcess("cache")
        writer_connection = _FakeConnection(clock)
        cache_connection = _FakeConnection(clock)

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            failures = quickstart._cleanup(
                [
                    ("writer", writer, writer_connection),
                    ("cache", cache, cache_connection),
                ],
                deadline=2.0,
                allow_forced=True,
            )

        self.assertEqual(failures, [])
        first_terminate_join = min(
            index
            for index, event in enumerate(events)
            if event.startswith("terminate-join:")
        )
        last_terminate = max(
            index
            for index, event in enumerate(events)
            if event.startswith("terminate:")
        )
        first_kill_join = min(
            index
            for index, event in enumerate(events)
            if event.startswith("kill-join:")
        )
        last_kill = max(
            index for index, event in enumerate(events) if event.startswith("kill:")
        )
        self.assertLess(last_terminate, first_terminate_join)
        self.assertLess(last_kill, first_kill_join)
        for process, connection in (
            (writer, writer_connection),
            (cache, cache_connection),
        ):
            self.assertFalse(process.is_alive())
            self.assertEqual(process.terminate_calls, 1)
            self.assertEqual(process.kill_calls, 1)
            self.assertTrue(
                any(
                    event == f"kill-join:{process.label}"
                    for event in events
                )
            )
            self.assertTrue(connection.closed)
        self.assertLessEqual(clock.now, 2.0 + 1e-9)

    def test_cleanup_polls_pending_acknowledgements_fairly(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        writer = _FakeProcess(clock, natural_exit_at=0.3)
        writer_connection = _FakeConnection(clock, packet_at=0.2)
        cache = _FakeProcess(clock)
        cache_connection = _FakeConnection(clock)

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            failures = quickstart._cleanup(
                [
                    ("writer", writer, writer_connection),
                    ("cache", cache, cache_connection),
                ],
                deadline=2.0,
            )

        self.assertEqual(len(failures), 1)
        self.assertRegex(failures[0], r"cleanup cache:.*timed out waiting")
        self.assertEqual(len(writer_connection.recv_times), 1)
        self.assertLessEqual(writer_connection.recv_times[0], 0.25)
        self.assertFalse(writer.is_alive())
        self.assertEqual(writer.terminate_calls, 0)
        self.assertEqual(cache.terminate_calls, 1)
        self.assertFalse(cache.is_alive())
        self.assertTrue(writer_connection.closed)
        self.assertTrue(cache_connection.closed)

    def test_wait_packet_rejects_ack_observed_after_deadline(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        process = _FakeProcess(clock)
        connection = _FakeConnection(clock, packet_at=1.0)
        clock.advance(1.01)

        with self.assertRaisesRegex(TimeoutError, r"timed out waiting for writer"):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._wait_packet(connection, process, "writer", 1.0)

        self.assertFalse(connection.received)

        clock = _FakeClock()
        process = _FakeProcess(clock)
        connection = _FakeConnection(
            clock,
            packet_at=1.01,
            poll_overshoot=0.02,
        )
        clock.advance(0.99)
        with self.assertRaisesRegex(TimeoutError, r"timed out waiting for writer"):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._wait_packet(connection, process, "writer", 1.0)
        self.assertTrue(connection.received)
        self.assertAlmostEqual(clock.now, 1.01)

    def test_stop_reports_exit_pipe_and_invalid_ack_failures(self) -> None:
        quickstart = _load_quickstart()

        clock = _FakeClock()
        process = _FakeProcess(clock, alive=False, exitcode=9)
        connection = _FakeConnection(clock)
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"no STOP acknowledgement; exit 9"
        ):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._stop(process, connection, "writer", 2.0)
        self.assertFalse(process.is_alive())
        self.assertTrue(connection.closed)

        clock = _FakeClock()
        process = _FakeProcess(clock, natural_exit_at=0.2)
        connection = _FakeConnection(clock)
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"STOP handshake failed.*exited with 0"
        ):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._stop(process, connection, "writer", 2.0)
        self.assertEqual(connection.sent, [("STOP", ())])
        self.assertFalse(process.is_alive())
        self.assertEqual(process.terminate_calls, 0)
        self.assertEqual(process.kill_calls, 0)
        self.assertTrue(connection.closed)

        clock = _FakeClock()
        process = _FakeProcess(clock, natural_exit_at=0.2)
        connection = _FakeConnection(clock, send_error=BrokenPipeError("closed"))
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"STOP handshake failed.*closed"
        ):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._stop(process, connection, "writer", 2.0)
        self.assertFalse(process.is_alive())
        self.assertTrue(connection.closed)

        clock = _FakeClock()
        process = _FakeProcess(clock, natural_exit_at=0.2)
        connection = _FakeConnection(
            clock, packet_at=0.1, packet=("INVALID", "unexpected")
        )
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"invalid STOP response INVALID: unexpected"
        ):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._stop(process, connection, "writer", 2.0)
        self.assertFalse(process.is_alive())
        self.assertTrue(connection.closed)

    def test_stop_exhaustion_reaps_and_reports_missing_ack(self) -> None:
        quickstart = _load_quickstart()

        clock = _FakeClock()
        process = _FakeProcess(clock)
        connection = _FakeConnection(clock)
        with self.assertRaisesRegex(
            quickstart.WorkerFailure, r"STOP handshake failed.*timed out waiting"
        ):
            with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
                quickstart._stop(process, connection, "writer", 2.0)
        self.assertFalse(process.is_alive())
        self.assertEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 0)
        self.assertTrue(connection.closed)
        self.assertLessEqual(clock.now, 2.0 + 1e-9)

    def test_stop_allowed_fallback_reaps_after_terminate_or_kill(self) -> None:
        quickstart = _load_quickstart()

        for terminate_exits, expected_kills in ((True, 0), (False, 1)):
            with self.subTest(terminate_exits=terminate_exits):
                clock = _FakeClock()
                process = _FakeProcess(clock, terminate_exits=terminate_exits)
                connection = _FakeConnection(clock)
                with mock.patch.object(
                    quickstart.time, "monotonic", clock.monotonic
                ):
                    quickstart._stop(
                        process,
                        connection,
                        "writer",
                        2.0,
                        allow_forced=True,
                    )
                self.assertFalse(process.is_alive())
                self.assertEqual(process.terminate_calls, 1)
                self.assertEqual(process.kill_calls, expected_kills)
                self.assertTrue(connection.closed)
                self.assertLessEqual(clock.now, 2.0 + 1e-9)

    def test_stop_uses_kill_after_terminate_raises(self) -> None:
        quickstart = _load_quickstart()
        clock = _FakeClock()
        process = _FakeProcess(
            clock,
            terminate_exits=False,
            terminate_error=PermissionError("terminate denied"),
        )
        connection = _FakeConnection(clock)

        with mock.patch.object(quickstart.time, "monotonic", clock.monotonic):
            quickstart._stop(
                process,
                connection,
                "writer",
                2.0,
                allow_forced=True,
            )

        self.assertFalse(process.is_alive())
        self.assertEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 1)
        self.assertTrue(connection.closed)
        self.assertLessEqual(clock.now, 2.0 + 1e-9)

    def test_primary_failure_precedes_cleanup_diagnostics(self) -> None:
        quickstart = _load_quickstart()
        stderr = io.StringIO()
        with (
            mock.patch.dict(quickstart.os.environ, {}, clear=True),
            mock.patch.object(
                quickstart, "_spawn", side_effect=RuntimeError("primary failed")
            ),
            mock.patch.object(
                quickstart,
                "_cleanup",
                return_value=["cleanup writer: forced cleanup failed"],
            ),
            contextlib.redirect_stderr(stderr),
        ):
            result = quickstart.run_example(
                "test/prefix",
                writer_target=lambda *_args: None,
                writer_args_factory=lambda _sid, _key: (),
                cache_target=lambda *_args: None,
                cache_args_factory=lambda _sid, _key: (),
                put_args_factory=lambda _sid, _key, _value: (),
                check_command="GET",
                observe=lambda _value: False,
                marker="SHOULD_NOT_PRINT",
            )

        self.assertEqual(result, 1)
        self.assertEqual(
            stderr.getvalue().splitlines(),
            [
                "example failed: primary failed",
                "cleanup: cleanup writer: forced cleanup failed",
            ],
        )

    def test_raw_example_has_no_sitos_or_numpy_import(self) -> None:
        path = REQUIRED["raw-zenoh"]
        self.assertEqual(
            _import_roots(path),
            {"__future__", "importlib", "re", "struct", "sys", "time", "typing", "zenoh"},
        )
        source = path.read_text(encoding="utf-8")
        self.assertIn("--prefix", source)
        self.assertIn("zenoh/bytes;sitos.v1", source)
        self.assertIn("PYTHON_RAW_ZENOH_OK", source)

    def test_wheel_guard_rejects_example_fixture_and_build_leaks(self) -> None:
        spec = importlib.util.spec_from_file_location("check_wheel", ROOT / "scripts" / "check_wheel.py")
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        leaks = (
            "examples/python/quickstart.py",
            "quickstart.py",
            "numpy_lut.py",
            "raw_zenoh.py",
            "test_python_examples.py",
            "sitos_raw_zenoh_fixture",
            "sitos_python_param_store_fixture",
            "build/example.o",
        )
        for member in leaks:
            with self.assertRaises(RuntimeError, msg=member):
                module.validate_wheel_members([member])

    def test_raw_constants_and_cli_are_exact(self) -> None:
        path = REQUIRED["raw-zenoh"]
        source = path.read_text(encoding="utf-8")
        self.assertIn('bytes([2]) + struct.pack("<d", 240.0)', source)
        self.assertIn('_PREFIX_CHUNK', source)
        self.assertLess(source.index('with zenoh.open'), source.index('print("PYTHON_RAW_ZENOH_OK")'))
        namespace: dict[str, object] = {}
        exec(compile(source, str(path), "exec"), namespace)
        parse = namespace["_parse_prefix"]
        self.assertEqual(parse(["--prefix", "safe/a_b.1-2"]), "safe/a_b.1-2")
        invalid = (
            [],
            ["--prefix"],
            ["--prefix", ""],
            ["--prefix", "a", "--prefix", "b"],
            ["--other", "a"],
            ["--prefix", "a//b"],
            ["--prefix", "a/*"],
            ["--prefix", "a b"],
        )
        for argv in invalid:
            with self.assertRaises(ValueError, msg=str(argv)):
                parse(argv)

    def test_numpy_contract_is_explicit_and_mutation_resistant(self) -> None:
        source = REQUIRED["numpy-lut"].read_text(encoding="utf-8")
        for required in (
            'dtype("<f4")',
            "1.5",
            "-2.25",
            "3.75",
            "0.5",
            "get_array",
            "shares_memory",
            "writeable",
            "metadata",
            "0000c03f000010c0000070400000003f",
        ):
            self.assertIn(required, source)
        namespace: dict[str, object] = {}
        examples_path = str(EXAMPLES)
        sys.path.insert(0, examples_path)
        try:
            exec(compile(source, str(REQUIRED["numpy-lut"]), "exec"), namespace)
        finally:
            sys.path.remove(examples_path)
        values = namespace["_VALUES"]
        self.assertEqual(values, (1.5, -2.25, 3.75, 0.5))
        self.assertEqual(namespace["_GOLDEN_BYTES"], struct.pack("<4f", *values))

        class View:
            def __init__(self, pointer: int) -> None:
                self.__array_interface__ = {"data": (pointer, True)}

        first_old, first_new = View(1), View(2)
        stable_first, stable_second = View(3), View(3)

        class Cache:
            def __init__(self) -> None:
                self.views = iter((first_old, first_new, stable_first, stable_second))

            def get_array(self, _key: str, *, dtype: object) -> View:
                del dtype
                return next(self.views)

        fake_numpy = types.SimpleNamespace(
            shares_memory=lambda left, right: (
                left.__array_interface__["data"][0]
                == right.__array_interface__["data"][0]
            )
        )
        pair = namespace["_stable_views"](
            Cache(), "key", object(), fake_numpy, LookupError
        )
        self.assertEqual(pair, (stable_first, stable_second))

    def test_raw_runtime_guard_and_key_are_independent(self) -> None:
        path = REQUIRED["raw-zenoh"]
        namespace: dict[str, object] = {}
        exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), namespace)
        expected_key = "independent/prefix/base/examples/fov"
        expected_payload = bytes([2]) + struct.pack("<d", 240.0)
        sessions: list[object] = []

        class Payload:
            def to_bytes(self) -> bytes:
                return expected_payload

        class Session:
            def __init__(self) -> None:
                self.timeouts: list[float] = []

            def __enter__(self) -> object:
                sessions.append(self)
                return self

            def __exit__(self, *args: object) -> None:
                return None

            def put(self, key: str, payload: bytes, *, encoding: object) -> None:
                self.put_args = (key, payload, str(encoding))

            def get(self, key: str, *, timeout: float) -> list[object]:
                self.timeouts.append(timeout)
                sample = types.SimpleNamespace(
                    key_expr=expected_key,
                    payload=Payload(),
                    encoding="zenoh/bytes;sitos.v1",
                )
                return [types.SimpleNamespace(err=None, ok=sample)]

        fake_zenoh = types.SimpleNamespace(
            Config=lambda: object(),
            Encoding=lambda value: value,
            open=lambda _config: Session(),
        )
        run = namespace["run"]
        with mock.patch.dict(sys.modules, {"zenoh": fake_zenoh}):
            with mock.patch("importlib.metadata.version", return_value="1.8.0"):
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(run("independent/prefix"), 1)
            with mock.patch("importlib.metadata.version", return_value="1.9.0"):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(run("independent/prefix"), 0)
        self.assertEqual(output.getvalue().strip(), "PYTHON_RAW_ZENOH_OK")
        self.assertEqual(
            sessions[-1].put_args,
            (expected_key, expected_payload, "zenoh/bytes;sitos.v1"),
        )
        self.assertLessEqual(sessions[-1].timeouts[0], 0.5)

        # A final query is capped to the actual time remaining, not a fresh
        # half-second timeout that can overrun the raw example deadline.
        sessions.clear()
        clock = iter((10.0, 17.8))
        real_time = namespace["time"]
        namespace["time"] = types.SimpleNamespace(monotonic=lambda: next(clock))
        try:
            with mock.patch.dict(sys.modules, {"zenoh": fake_zenoh}):
                with mock.patch("importlib.metadata.version", return_value="1.9.0"):
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(run("independent/prefix"), 0)
        finally:
            namespace["time"] = real_time
        self.assertAlmostEqual(sessions[-1].timeouts[0], 0.2)

    def test_driver_requires_case_and_isolates_quickstart_contracts(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                _parse_args([])
        self.assertEqual(caught.exception.code, 2)
        self.assertNotIn(
            "test_wheel_guard_rejects_example_fixture_and_build_leaks",
            _contract_methods("quickstart"),
        )


class PythonExampleExecutionTest(unittest.TestCase):
    def test_quickstart(self) -> None:
        result = _run_script("quickstart", deadline=time.monotonic() + CASE_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], MARKERS["quickstart"])
        self.assertNotIn("SITOS_EXAMPLE_TEST_", result.stdout + result.stderr)

    def test_numpy_lut(self) -> None:
        result = _run_script("numpy-lut", deadline=time.monotonic() + CASE_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], MARKERS["numpy-lut"])
        self.assertIn("shape and dtype metadata are not transported", result.stdout)
        self.assertNotIn("SITOS_EXAMPLE_TEST_", result.stdout + result.stderr)

    def test_raw_zenoh(self) -> None:
        result = _run_raw_with_peer(time.monotonic() + CASE_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], MARKERS["raw-zenoh"])
        self.assertNotIn("SITOS_EXAMPLE_TEST_", result.stdout + result.stderr)

    def test_failure_cleanup(self) -> None:
        record = re.compile(
            r"^SITOS_EXAMPLE_TEST_CHILD (node|writer|cache) ([1-9][0-9]*)$"
        )
        failure_record = re.compile(
            r"^SITOS_EXAMPLE_TEST_FAILURE cache ([1-9][0-9]*) test-injected cache startup failure$"
        )
        for name in ("quickstart", "numpy-lut"):
            result = _run_script(
                name,
                deadline=time.monotonic() + CASE_SECONDS,
                environment={"SITOS_EXAMPLE_TEST_FAIL": "cache-before-open"},
            )
            self.assertEqual(result.returncode, 70, result.stderr)
            cleanup_line = "SITOS_EXAMPLE_TEST_CLEANUP_OK cache-before-open"
            self.assertEqual(result.stderr.splitlines().count(cleanup_line), 1)
            self.assertNotIn(MARKERS[name], result.stdout)
            records = [
                record.fullmatch(line)
                for line in result.stderr.splitlines()
                if line.startswith("SITOS_EXAMPLE_TEST_CHILD")
            ]
            self.assertTrue(records and all(records), result.stderr)
            pids = {match.group(1): int(match.group(2)) for match in records if match}
            self.assertEqual(set(pids), {"node", "writer", "cache"}, result.stderr)
            self.assertEqual(len(records), 3, result.stderr)
            failures = [
                failure_record.fullmatch(line)
                for line in result.stderr.splitlines()
                if line.startswith("SITOS_EXAMPLE_TEST_FAILURE")
            ]
            self.assertEqual(len(failures), 1, result.stderr)
            self.assertEqual(int(failures[0].group(1)), pids["cache"])
            self.assertTrue(all(not _pid_alive(pid) for pid in pids.values()))
            invalid = _run_script(
                name,
                deadline=time.monotonic() + CASE_SECONDS,
                environment={"SITOS_EXAMPLE_TEST_FAIL": "invalid"},
            )
            self.assertEqual(invalid.returncode, 2, invalid.stderr)
            self.assertNotIn("SITOS_EXAMPLE_TEST_CHILD", invalid.stderr)


def wheel_boundary(wheel: Path) -> int:
    spec = importlib.util.spec_from_file_location("check_wheel", ROOT / "scripts" / "check_wheel.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with zipfile.ZipFile(wheel) as archive:
        try:
            module.validate_wheel_members(archive.namelist())
        except RuntimeError as error:
            print(str(error), file=sys.stderr)
            return 1
    return 0


CASES = (
    "contract",
    "quickstart",
    "numpy-lut",
    "raw-zenoh",
    "failure-cleanup",
    "wheel-boundary",
)


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=CASES)
    parser.add_argument("--wheel", type=Path)
    args = parser.parse_args(argv)
    if args.case == "wheel-boundary" and args.wheel is None:
        parser.error("wheel-boundary requires --wheel")
    return args


def _contract_methods(case: str) -> tuple[str, ...]:
    common = (
        "test_required_example_files_exist",
        "test_required_success_markers_are_declared",
    )
    process_contracts = (
        "test_process_examples_declare_lifecycle_contract",
        "test_pipe_failures_keep_stage_and_exit_diagnostics",
        "test_near_expiry_does_not_spawn_a_coordinator",
        "test_cleanup_requests_all_stops_before_shared_deadline_wait",
        "test_cleanup_reports_exit_between_liveness_check_and_stop",
        "test_cleanup_rejects_ack_observed_after_shared_deadline",
        "test_cleanup_escalates_and_reaps_all_survivors_by_phase",
        "test_cleanup_polls_pending_acknowledgements_fairly",
        "test_wait_packet_rejects_ack_observed_after_deadline",
        "test_stop_reports_exit_pipe_and_invalid_ack_failures",
        "test_stop_exhaustion_reaps_and_reports_missing_ack",
        "test_stop_allowed_fallback_reaps_after_terminate_or_kill",
        "test_stop_uses_kill_after_terminate_raises",
        "test_primary_failure_precedes_cleanup_diagnostics",
    )
    if case in {"quickstart", "failure-cleanup"}:
        return common + process_contracts
    if case == "numpy-lut":
        return common + process_contracts + (
            "test_numpy_contract_is_explicit_and_mutation_resistant",
        )
    if case == "raw-zenoh":
        return common + (
            "test_raw_example_has_no_sitos_or_numpy_import",
            "test_raw_constants_and_cli_are_exact",
            "test_raw_runtime_guard_and_key_are_independent",
            "test_raw_primary_and_cleanup_failures_are_both_preserved",
        )
    raise ValueError(f"no selected contracts for {case}")


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if args.case == "wheel-boundary":
        return wheel_boundary(args.wheel)
    if args.case == "contract":
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(
            PythonExampleContractTest
        )
    else:
        suite = unittest.TestSuite(
            PythonExampleContractTest(method) for method in _contract_methods(args.case)
        )
        execution_method = {
            "quickstart": "test_quickstart",
            "numpy-lut": "test_numpy_lut",
            "raw-zenoh": "test_raw_zenoh",
            "failure-cleanup": "test_failure_cleanup",
        }[args.case]
        suite.addTest(PythonExampleExecutionTest(execution_method))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
