"""Process-isolated support for raw zenoh-python interoperability tests."""

from __future__ import annotations

import json
import os
import queue
import socket
import subprocess
import threading
import uuid
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import TextIO

import zenoh

CANONICAL_SITOS_ENCODING = "zenoh/bytes;sitos.v1"
START_ATTEMPTS = 3


@dataclass(frozen=True)
class WireSample:
    key: str
    payload: bytes
    encoding: str


def copy_sample(sample: zenoh.Sample) -> WireSample:
    return WireSample(
        key=str(sample.key_expr),
        payload=sample.payload.to_bytes(),
        encoding=str(sample.encoding),
    )


def assert_wire_sample(
    sample: WireSample,
    *,
    expected_key: str,
    expected_payload: bytes,
    expected_encoding: str,
) -> None:
    assert sample.key == expected_key
    assert sample.payload == expected_payload
    assert sample.encoding == expected_encoding


class FixtureProcess:
    """Own a bounded C++ fixture process and its line protocol."""

    def __init__(self) -> None:
        executable = os.environ.get("SITOS_RAW_ZENOH_FIXTURE")
        if executable is None or not Path(executable).is_file():
            raise AssertionError("SITOS_RAW_ZENOH_FIXTURE must name the built fixture")
        token = f"{os.getpid()}_{uuid.uuid4().hex}"
        self.prefix = f"sitos/interop_{token}"
        self.session_id = f"session_{token}"
        self.output: list[str] = []
        self.stderr_output: list[str] = []
        self._stderr_lock = threading.Lock()
        startup_errors: list[str] = []

        for attempt in range(1, START_ATTEMPTS + 1):
            self.port = self._select_port()
            self._start(executable, attempt)
            try:
                ready = self._readline(10.0)
                expected = f"READY {self.prefix} {self.port}"
                if ready != expected:
                    raise AssertionError(
                        f"expected {expected!r}, received {ready!r}; "
                        f"{self._diagnostics()}"
                    )
                return
            except AssertionError as startup_error:
                startup_errors.append(str(startup_error))
                try:
                    self.close()
                except AssertionError as cleanup_error:
                    startup_errors.append(str(cleanup_error))
                if self._process.returncode == 3 and attempt < START_ATTEMPTS:
                    continue
                details = "; ".join(startup_errors)
                raise AssertionError(f"fixture startup failed: {details}") from startup_error
        raise AssertionError("fixture startup retry limit reached")

    @staticmethod
    def _select_port() -> int:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            return probe.getsockname()[1]

    def _start(self, executable: str, attempt: int) -> None:
        self._stdout_lines: queue.Queue[str | None] = queue.Queue()
        self._process = subprocess.Popen(
            [executable, self.prefix, str(self.port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert self._process.stdout is not None
        assert self._process.stderr is not None
        self._stdout_reader = threading.Thread(
            target=self._drain_stdout,
            args=(self._process.stdout, self._stdout_lines),
            daemon=True,
        )
        self._stderr_reader = threading.Thread(
            target=self._drain_stderr,
            args=(attempt, self._process.stderr),
            daemon=True,
        )
        self._stdout_reader.start()
        self._stderr_reader.start()

    @staticmethod
    def _drain_stdout(stream: TextIO, lines: queue.Queue[str | None]) -> None:
        for line in stream:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)

    def _drain_stderr(self, attempt: int, stream: TextIO) -> None:
        for line in stream:
            with self._stderr_lock:
                self.stderr_output.append(f"attempt {attempt}: {line.rstrip()}")

    def _diagnostics(self) -> str:
        with self._stderr_lock:
            stderr_output = list(self.stderr_output)
        return f"stdout={self.output!r}; stderr={stderr_output!r}"

    def _readline(self, timeout: float) -> str:
        try:
            line = self._stdout_lines.get(timeout=timeout)
        except queue.Empty as error:
            raise AssertionError(
                f"fixture produced no line within {timeout:g} seconds; "
                f"{self._diagnostics()}"
            ) from error
        if line is None:
            self._process.poll()
            raise AssertionError(
                f"fixture exited with {self._process.returncode}; {self._diagnostics()}"
            )
        self.output.append(line)
        return line

    def command(self, command: str, expected: str, timeout: float = 5.0) -> None:
        if self._process.poll() is not None or self._process.stdin is None:
            raise AssertionError(f"fixture is not running; {self._diagnostics()}")
        try:
            self._process.stdin.write(command + "\n")
            self._process.stdin.flush()
        except OSError as error:
            raise AssertionError(
                f"fixture command pipe failed; {self._diagnostics()}"
            ) from error
        actual = self._readline(timeout)
        if actual != expected:
            raise AssertionError(
                f"command {command!r} expected {expected!r}, received {actual!r}; "
                f"{self._diagnostics()}"
            )

    def put_dp(self, key: str, value: float) -> None:
        self.command(f"PUT_DP {key} {value:.17g}", f"PUT_OK {key}")

    def create_session(self) -> None:
        self.command(
            f"CREATE_SESSION {self.session_id}", f"SESSION_OK {self.session_id}"
        )

    def open_raw_session(self) -> zenoh.Session:
        config = zenoh.Config.from_json5(
            json.dumps(
                {
                    "mode": "client",
                    "connect": {"endpoints": [f"tcp/127.0.0.1:{self.port}"]},
                    "scouting": {"multicast": {"enabled": False}},
                }
            )
        )
        return zenoh.open(config)

    def close(self) -> None:
        cleanup_error: str | None = None
        if self._process.poll() is None and self._process.stdin is not None:
            try:
                self._process.stdin.write("STOP\n")
                self._process.stdin.flush()
                stopped = self._readline(5.0)
                if stopped != "STOPPED":
                    cleanup_error = f"expected 'STOPPED', received {stopped!r}"
            except (AssertionError, OSError) as error:
                cleanup_error = str(error)
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=5)
            cleanup_error = cleanup_error or "fixture required forced termination"
        self._stdout_reader.join(timeout=1)
        self._stderr_reader.join(timeout=1)
        if self._process.stdin is not None:
            try:
                self._process.stdin.close()
            except OSError:
                pass
        if self._process.returncode != 0:
            cleanup_error = cleanup_error or f"fixture exited with {self._process.returncode}"
        if cleanup_error is not None:
            raise AssertionError(f"{cleanup_error}; {self._diagnostics()}")

    def __enter__(self) -> FixtureProcess:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        try:
            self.close()
        except AssertionError as cleanup_error:
            if exc_value is None:
                raise
            if hasattr(exc_value, "add_note"):
                exc_value.add_note(f"fixture cleanup failed: {cleanup_error}")
                return
            raise cleanup_error from exc_value
