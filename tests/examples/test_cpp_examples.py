"""Standard-library acceptance driver for the C++ examples."""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Iterable, TextIO

READY_PREFIX = "SITOBOLON_READY "
DIAGNOSTIC_LIMIT = 12_000
PENDING_LIMIT = 4_096
CHUNK_SIZE = 4_096
MODE_SECONDS = 45.0
START_SECONDS = 8.0
CLEANUP_SECONDS = 4.0
MIN_LAUNCH_WORK_SECONDS = START_SECONDS * 2.0
START_ATTEMPTS = 5


class ChildExitedBeforeReady(AssertionError):
    """The child positively closed its pipes before publishing readiness."""

    def __init__(self, returncode: int | None, diagnostics: str) -> None:
        super().__init__(f"child exited with {returncode} before readiness; {diagnostics}")
        self.returncode = returncode


def _token() -> str:
    return f"{os.getpid()}_{uuid.uuid4().hex}"


def _valid_prefix() -> str:
    return f"sitos/example_{_token()}"


def _port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _config() -> tuple[str, str]:
    marker = f"SITOBOLON_CONFIG_MARKER_{_token()}"
    # This is a complete Zenoh document, not a fragment for sitobolon to merge.
    config = (
        f"// {marker}\n"
        f'{{"mode":"peer","listen":{{"endpoints":["tcp/127.0.0.1:{_port()}"]}},'
        '"scouting":{"multicast":{"enabled":false}}}'
    )
    return config, marker


def _remaining(deadline: float) -> float:
    return max(0.0, deadline - time.monotonic())


def _send_ctrl_c_helper(pid: int) -> int:
    """Deliver Ctrl-C from a helper attached to the target's private console."""
    if os.name != "nt":
        return 2
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.FreeConsole.restype = ctypes.c_int
    kernel32.AttachConsole.argtypes = [ctypes.c_uint32]
    kernel32.AttachConsole.restype = ctypes.c_int
    kernel32.SetConsoleCtrlHandler.argtypes = [ctypes.c_void_p, ctypes.c_int]
    kernel32.SetConsoleCtrlHandler.restype = ctypes.c_int
    kernel32.GenerateConsoleCtrlEvent.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    kernel32.GenerateConsoleCtrlEvent.restype = ctypes.c_int
    kernel32.GetLastError.restype = ctypes.c_uint32
    invalid_handle = 6  # ERROR_INVALID_HANDLE: helper simply had no inherited console.
    if not kernel32.FreeConsole() and ctypes.get_last_error() != invalid_handle:
        return 1
    if not kernel32.AttachConsole(pid):
        return 1
    if not kernel32.SetConsoleCtrlHandler(None, 1):
        kernel32.FreeConsole()
        return 1
    generated = kernel32.GenerateConsoleCtrlEvent(0, 0)  # CTRL_C_EVENT, process group 0.
    detached = kernel32.FreeConsole()
    if not generated or not detached:
        return 1
    return 0


def _send_ctrl_c(pid: int, deadline: float) -> None:
    timeout = min(START_SECONDS, _remaining(deadline))
    if timeout <= 0:
        raise AssertionError("insufficient time to invoke Ctrl-C helper")
    result = _run(
        [sys.executable, str(Path(__file__).resolve()), "send-ctrl-c", str(pid)],
        timeout=timeout,
    )
    if result.returncode != 0:
        raise AssertionError(f"Ctrl-C helper failed: {result.stdout + result.stderr!r}")


class Child:
    """Own a child, drain bounded binary chunks, and always reap it."""

    def __init__(
        self,
        argv: list[str],
        marker: str,
        *,
        new_group: bool = False,
        windows_event: str | None = None,
    ) -> None:
        flags = 0
        if os.name == "nt":
            if windows_event == "ctrl-c":
                flags = subprocess.CREATE_NEW_CONSOLE
            elif windows_event == "ctrl-break" or new_group:
                flags = subprocess.CREATE_NEW_PROCESS_GROUP
        self.process = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            creationflags=flags,
        )
        self.lines: queue.Queue[str | None] = queue.Queue(maxsize=16)
        self._out_tail = bytearray()
        self._err_tail = bytearray()
        self._marker = marker.encode("utf-8")
        self._out_marker_tail = bytearray()
        self._err_marker_tail = bytearray()
        self._forbidden_seen = False
        self._lock = threading.Lock()
        self._threads: list[threading.Thread] = []
        self._stdout_stream = self.process.stdout
        self._stderr_stream = self.process.stderr
        try:
            if self._stdout_stream is None or self._stderr_stream is None:
                raise RuntimeError("child output pipes were not created")
            candidates = [
                threading.Thread(target=self._stdout, args=(self._stdout_stream,), daemon=True),
                threading.Thread(target=self._stderr, args=(self._stderr_stream,), daemon=True),
            ]
            for thread in candidates:
                thread.start()
                # Append only after start succeeds; _join never joins an
                # unstarted thread when setup is interrupted.
                self._threads.append(thread)
        except BaseException:
            self.cleanup()
            raise

    @property
    def forbidden_seen(self) -> bool:
        with self._lock:
            return self._forbidden_seen

    def _record(self, target: bytearray, scan_tail: bytearray, chunk: bytes) -> None:
        with self._lock:
            probe = bytes(scan_tail) + chunk
            if self._marker in probe:
                self._forbidden_seen = True
            keep = max(0, len(self._marker) - 1)
            scan_tail[:] = probe[-keep:] if keep else b""
            target.extend(chunk)
            if len(target) > DIAGNOSTIC_LIMIT:
                del target[: len(target) - DIAGNOSTIC_LIMIT]

    def _publish(self, line: str) -> None:
        try:
            self.lines.put_nowait(line)
        except queue.Full:
            # A bounded queue deliberately drops excess noise. The diagnostic
            # tail still records it, and readiness cannot be inferred from it.
            pass

    def _stdout(self, stream: TextIO) -> None:
        pending = b""
        discarding = False
        try:
            while True:
                chunk = stream.read(CHUNK_SIZE)
                if not chunk:
                    break
                self._record(self._out_tail, self._out_marker_tail, chunk)
                if discarding:
                    newline = chunk.find(b"\n")
                    if newline < 0:
                        continue
                    chunk = chunk[newline + 1 :]
                    discarding = False
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    self._publish(raw[:PENDING_LIMIT].rstrip(b"\r").decode("utf-8", "replace"))
                if len(pending) > PENDING_LIMIT:
                    self._publish("<stdout line exceeded bounded pending storage>")
                    pending = b""
                    discarding = True
            if pending:
                self._publish(pending[:PENDING_LIMIT].rstrip(b"\r").decode("utf-8", "replace"))
        finally:
            self._publish(None)

    def _stderr(self, stream: TextIO) -> None:
        while True:
            chunk = stream.read(CHUNK_SIZE)
            if not chunk:
                return
            self._record(self._err_tail, self._err_marker_tail, chunk)

    def diagnostics(self) -> str:
        with self._lock:
            stdout = bytes(self._out_tail).decode("utf-8", "replace")
            stderr = bytes(self._err_tail).decode("utf-8", "replace")
            marker = self._forbidden_seen
        return f"stdout={stdout!r} stderr={stderr!r} forbidden_marker={marker}"

    def ready(self, prefix: str, deadline: float) -> None:
        expected = READY_PREFIX + prefix
        while True:
            timeout = _remaining(deadline)
            if timeout <= 0:
                raise AssertionError(f"readiness timeout; {self.diagnostics()}")
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty as error:
                raise AssertionError(f"readiness timeout; {self.diagnostics()}") from error
            if line is None:
                code = self.process.poll()
                if code is None:
                    try:
                        waited_code = self.process.wait(timeout=_remaining(deadline))
                    except subprocess.TimeoutExpired:
                        waited_code = None
                    code = waited_code if waited_code is not None else self.process.returncode
                raise ChildExitedBeforeReady(code, self.diagnostics())
            if line != expected:
                raise AssertionError(f"unexpected stdout line {line!r}; {self.diagnostics()}")
            return

    def stop(self, event: int | str | None, deadline: float) -> None:
        if self.process.poll() is None:
            if event == "ctrl-c":
                _send_ctrl_c(self.process.pid, deadline)
            elif event == "ctrl-break":
                self.process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.process.send_signal(signal.SIGINT if event is None else event)
        self.wait_zero(deadline)

    def wait_zero(self, deadline: float) -> None:
        try:
            self.process.wait(timeout=_remaining(deadline))
        except subprocess.TimeoutExpired as error:
            self.cleanup()
            raise AssertionError(f"child did not stop; {self.diagnostics()}") from error
        if self.process.returncode != 0:
            raise AssertionError(f"child returned {self.process.returncode}; {self.diagnostics()}")
        self._join(deadline)

    def _terminate(self, deadline: float) -> None:
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=max(0.1, min(2.0, _remaining(deadline))))
        except subprocess.TimeoutExpired:
            self.process.kill()
            try:
                self.process.wait(timeout=max(0.1, min(2.0, _remaining(deadline))))
            except subprocess.TimeoutExpired as error:
                raise AssertionError("child could not be reaped after kill") from error
        if self.process.poll() is None:
            raise AssertionError("child remained unreaped after termination")

    def _join(self, deadline: float) -> None:
        timeout = max(0.1, min(1.0, _remaining(deadline)))
        for thread in self._threads:
            thread.join(timeout=timeout)
            if thread.is_alive():
                raise AssertionError("output drainer did not terminate")

    def cleanup(self) -> None:
        # Cleanup owns an independent positive reserve, never the expired work
        # deadline. The 45-second mode budget plus this reserve stays below CTest 60.
        deadline = time.monotonic() + CLEANUP_SECONDS
        if self.process.poll() is None:
            self._terminate(deadline)
        self._join(deadline)
        for stream in (self._stdout_stream, self._stderr_stream):
            if stream is not None:
                stream.close()


def _run(argv: list[str], *, timeout: float = 8.0) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(argv, capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"command timed out: {argv!r}") from error


def _assert_no_ready(result: subprocess.CompletedProcess[bytes]) -> None:
    output = result.stdout + result.stderr
    assert READY_PREFIX.encode() not in output, result


def configure_guard(args: argparse.Namespace) -> int:
    build = Path(args.build)
    if build.exists():
        import shutil

        shutil.rmtree(build)
    result = _run(
        [
            args.cmake,
            "-S",
            args.source,
            "-B",
            str(build),
            "-G",
            args.generator,
            "-DSITOS_BUILD_EXAMPLES=ON",
            "-DSITOS_WITH_ZENOH=OFF",
            "-DSITOS_BUILD_TESTS=OFF",
        ],
        timeout=90.0,
    )
    output = result.stdout + result.stderr
    expected = b"SITOS_BUILD_EXAMPLES requires SITOS_WITH_ZENOH=ON"
    if result.returncode == 0 or expected not in output:
        raise AssertionError(
            f"configure guard mismatch (exit={result.returncode}); output={output!r}"
        )
    return 0


def _help(executable: str) -> int:
    result = _run([executable, "--help"])
    assert result.returncode == 0, result
    _assert_no_ready(result)
    text = result.stdout.decode("utf-8", "replace")
    section = text.split("Options:\n", 1)[-1]
    entries = re.findall(r"^  (--[a-z][a-z-]*)(?:\s|$)", section, re.MULTILINE)
    expected = {
        "--engine",
        "--rocksdb-path",
        "--prefix",
        "--zenoh-config",
        "--zenoh-config-file",
        "--help",
    }
    assert set(entries) == expected and len(entries) == len(expected), entries
    assert "inmemory|rocksdb" in text, text
    return 0


def _invalid(executable: str, argv: tuple[str, ...], forbidden: tuple[bytes, ...], deadline: float) -> None:
    timeout = min(8.0, _remaining(deadline))
    result = _run([executable, *argv], timeout=timeout)
    assert result.returncode != 0, (argv, result)
    _assert_no_ready(result)
    output = result.stdout + result.stderr
    for fragment in forbidden:
        assert fragment not in output, (argv, fragment, result)


def invalid_arguments(executable: str, rocksdb_built: bool) -> int:
    deadline = time.monotonic() + MODE_SECONDS
    with tempfile.TemporaryDirectory(prefix="sitos-example-invalid-") as root:
        root_path = Path(root)
        unreadable = root_path / "missing.json5"
        empty = root_path / "empty.json5"
        empty.touch()
        direct_marker = "DIRECT_SECRET_" + _token()
        file_marker = "FILE_SECRET_" + _token()
        duplicate_marker = "DUPLICATE_SECRET_" + _token()
        direct_json = '{"mode":"peer","secret":"' + direct_marker + '"'
        file_json = '{"mode":"peer","secret":"' + file_marker + '"'
        duplicate_json = '{"mode":"peer","secret":"' + duplicate_marker + '"}'
        malformed = root_path / "malformed.json5"
        malformed.write_text(file_json, encoding="utf-8")
        cases: list[tuple[tuple[str, ...], tuple[bytes, ...]]] = []

        def add(*argv: str, forbidden: tuple[str, ...] = ()) -> None:
            cases.append((tuple(argv), tuple(item.encode("utf-8") for item in forbidden)))

        add("--unknown")
        add("--help", "--unknown")
        add("--help", "--help")
        add("--help", "--engine", "inmemory")
        add("--help", "--rocksdb-path", "db")
        add("--help", "--prefix", "sitos/x")
        add("--help", "--zenoh-config", direct_json,
            forbidden=(direct_json, direct_marker))
        add("--help", "--zenoh-config-file", str(malformed),
            forbidden=(file_json, file_marker))
        add("--engine")
        add("--engine", "bogus")
        add("--engine", "inmemory", "--engine", "inmemory")
        add("--prefix")
        add("--prefix", "bad prefix")
        add("--prefix", "sitos//x")
        add("--prefix", "sitos?x")
        add("--prefix", "sitos/x", "--prefix", "sitos/y")
        add("--rocksdb-path")
        add("--rocksdb-path", "")
        add("--rocksdb-path", str(root_path / "db"), "--rocksdb-path", "db2")
        add("--rocksdb-path", str(root_path / "db"))
        add("--engine", "inmemory", "--rocksdb-path", "")
        add("--engine", "inmemory", "--rocksdb-path", str(root_path / "db"))
        add("--zenoh-config")
        add("--zenoh-config", "")
        add("--zenoh-config-file")
        add("--zenoh-config", duplicate_json, "--zenoh-config", duplicate_json,
            forbidden=(duplicate_json, duplicate_marker))
        add("--zenoh-config-file", str(empty), "--zenoh-config-file", str(malformed),
            forbidden=(file_json, file_marker))
        add("--zenoh-config", duplicate_json, "--zenoh-config-file", str(malformed),
            forbidden=(duplicate_json, duplicate_marker, file_json, file_marker))
        add("--zenoh-config-file", str(malformed), "--zenoh-config", direct_json,
            forbidden=(file_json, file_marker, direct_json, direct_marker))
        add("--zenoh-config-file", str(unreadable))
        add("--zenoh-config-file", str(empty))
        add("--zenoh-config", direct_json, forbidden=(direct_json, direct_marker))
        add("--zenoh-config-file", str(malformed), forbidden=(file_json, file_marker))
        if rocksdb_built:
            add("--engine", "rocksdb")
        else:
            rocksdb_path = root_path / "db-off"
            add("--engine", "rocksdb", "--rocksdb-path", str(rocksdb_path))
        for argv, forbidden in cases:
            _invalid(executable, argv, forbidden, deadline)
        if not rocksdb_built:
            assert not rocksdb_path.exists(), rocksdb_path
    return 0


def _launch(
    executable: str,
    config_file: bool,
    *,
    prefix: str,
    deadline: float,
    db: Path | None = None,
    event: int | str | None = None,
) -> None:
    fixed_args = ["--engine", "rocksdb", "--rocksdb-path", str(db)] if db else []
    for attempt in range(START_ATTEMPTS):
        if _remaining(deadline) < MIN_LAUNCH_WORK_SECONDS:
            raise AssertionError("insufficient lifecycle work budget for another child")
        config, marker = _config()
        argv = [executable, "--prefix", prefix, *fixed_args]
        temporary_path: Path | None = None
        if config_file:
            temporary = tempfile.NamedTemporaryFile(
                "w", suffix=".json5", delete=False, encoding="utf-8"
            )
            temporary.write(config)
            temporary.close()
            temporary_path = Path(temporary.name)
            argv += ["--zenoh-config-file", str(temporary_path)]
        else:
            argv += ["--zenoh-config", config]
        child = Child(
            argv,
            marker,
            new_group=(event == "ctrl-break"),
            windows_event=event if isinstance(event, str) else None,
        )
        try:
            try:
                child.ready(prefix, min(deadline, time.monotonic() + START_SECONDS))
            except ChildExitedBeforeReady as error:
                child.cleanup()
                # A failed attempt is fully reaped before either retry or raise;
                # a leaked marker is never allowed to disappear with the child.
                assert not child.forbidden_seen, child.diagnostics()
                if error.returncode == 3 and attempt + 1 < START_ATTEMPTS:
                    continue
                raise
            except BaseException:
                child.cleanup()
                assert not child.forbidden_seen, child.diagnostics()
                raise
            try:
                child.stop(event, min(deadline, time.monotonic() + START_SECONDS))
            except BaseException:
                child.cleanup()
                assert not child.forbidden_seen, child.diagnostics()
                raise
            assert not child.forbidden_seen, child.diagnostics()
            # Successful stop does not resend a signal; explicitly close the
            # already-reaped streams and drainer threads.
            child.cleanup()
            return
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)


def lifecycle(executable: str) -> int:
    deadline = time.monotonic() + MODE_SECONDS
    events = ["ctrl-c", "ctrl-break"] if os.name == "nt" else [signal.SIGINT]
    for config_file in (False, True):
        for event in events:
            for _ in range(2 if os.name == "nt" else 1):
                _launch(executable, config_file, prefix=_valid_prefix(), deadline=deadline, event=event)
    return 0


def rocksdb_lifecycle(executable: str) -> int:
    deadline = time.monotonic() + MODE_SECONDS
    event: int | str = "ctrl-break" if os.name == "nt" else signal.SIGINT
    with tempfile.TemporaryDirectory(prefix="sitos-example-rocksdb-") as root:
        path = Path(root) / "database"
        _launch(executable, False, prefix=_valid_prefix(), deadline=deadline, db=path, event=event)
        _launch(executable, True, prefix=_valid_prefix(), deadline=deadline, db=path, event=event)
        import shutil

        shutil.rmtree(path)
        assert not path.exists()
    return 0


def ctest_contract(args: argparse.Namespace) -> int:
    result = _run([args.ctest, "--test-dir", args.build, "--show-only=json-v1"], timeout=15.0)
    assert result.returncode == 0, result
    payload = json.loads(result.stdout.decode("utf-8"))
    expected = {
        "CppQuickstartRuns",
        "SitobolonHelpDocumentsOptions",
        "SitobolonRejectsInvalidArguments",
        "SitobolonStartsAndStopsCleanly",
    }
    if args.rocksdb_built:
        expected.add("SitobolonRocksDbReleasesPath")

    graph = payload.get("backtraceGraph", {})
    files = graph.get("files", [])
    nodes = graph.get("nodes", [])
    source_file = str(Path("examples/cpp/CMakeLists.txt").resolve())

    def registration_files(backtrace: int) -> set[str]:
        found: set[str] = set()
        seen: set[int] = set()
        while isinstance(backtrace, int) and backtrace not in seen and backtrace < len(nodes):
            seen.add(backtrace)
            node = nodes[backtrace]
            file_index = node.get("file")
            if isinstance(file_index, int) and file_index < len(files):
                found.add(str(Path(files[file_index]).resolve()))
            backtrace = node.get("parent")
        return found

    registered_from_examples = {
        test["name"]
        for test in payload.get("tests", [])
        if source_file in registration_files(test.get("backtrace"))
    }
    assert registered_from_examples == expected, (registered_from_examples, expected)
    serial = {"CppQuickstartRuns", "SitobolonStartsAndStopsCleanly"}
    if args.rocksdb_built:
        serial.add("SitobolonRocksDbReleasesPath")
    for test in payload["tests"]:
        if test["name"] not in expected:
            continue
        properties = {item["name"]: item["value"] for item in test.get("properties", [])}
        assert properties.get("TIMEOUT") == 60.0, (test["name"], properties)
        assert (properties.get("RUN_SERIAL") is True) == (test["name"] in serial), (test["name"], properties)
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument(
        "mode",
        choices=("configure-guard", "help", "invalid", "lifecycle", "rocksdb", "ctest-contract", "send-ctrl-c"),
    )
    parser.add_argument("helper_pid", nargs="?")
    parser.add_argument("--executable")
    parser.add_argument("--rocksdb-built", action="store_true")
    parser.add_argument("--source", default=".")
    parser.add_argument("--build", default="build/issue31-guard")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args(list(argv) if argv is not None else None)
    if args.mode == "configure-guard":
        return configure_guard(args)
    if args.mode == "ctest-contract":
        return ctest_contract(args)
    if args.mode == "send-ctrl-c":
        if args.helper_pid is None or os.name != "nt":
            return 2
        return _send_ctrl_c_helper(int(args.helper_pid))
    if not args.executable:
        parser.error("--executable is required for this mode")
    if args.mode == "help":
        return _help(args.executable)
    if args.mode == "invalid":
        return invalid_arguments(args.executable, args.rocksdb_built)
    if args.mode == "lifecycle":
        return lifecycle(args.executable)
    return rocksdb_lifecycle(args.executable)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"example validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
