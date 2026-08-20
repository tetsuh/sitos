#!/usr/bin/env python3
"""Process-isolated Python quickstart for public sitos APIs.

Each spawned role owns one public client. The coordinator owns no Zenoh session.
READY is local startup only, and PUT is retried because submission is not delivery.
"""
from __future__ import annotations

import multiprocessing
import os
import sys
import time
import uuid
from multiprocessing.connection import Connection
from typing import Any, Callable

WORK_SECONDS = 30.0
FAILURE_SECONDS = 15.0
CHILD_GRACEFUL_STOP_SECONDS = 5.0
TERMINATE_RESERVE_SECONDS = 0.5
KILL_RESERVE_SECONDS = 0.5
CHILD_CLEANUP_SECONDS = (
    CHILD_GRACEFUL_STOP_SECONDS
    + TERMINATE_RESERVE_SECONDS
    + KILL_RESERVE_SECONDS
)
CLEANUP_SECONDS = 19.0
FAILURE_ENV = "SITOS_EXAMPLE_TEST_FAIL"
FAILURE_VALUE = "cache-before-open"
FAILURE_MESSAGE = "test-injected cache startup failure"
FAILURE_CLEANUP_LINE = "SITOS_EXAMPLE_TEST_CLEANUP_OK cache-before-open"


class WorkerFailure(RuntimeError):
    """A worker reported a structured terminal failure."""


def _send_error(connection: Connection, error: BaseException) -> None:
    try:
        connection.send(("ERROR", f"{type(error).__name__}: {error}"))
    except (BrokenPipeError, EOFError, OSError):
        pass


def _node_worker(connection: Connection, prefix: str) -> None:
    node = None
    try:
        import sitos

        node = sitos.StorageNode(
            sitos.InMemoryEngine(), prefix=prefix, zenoh_config_json=None
        )
        connection.send(("READY", ""))
        while True:
            command, args = connection.recv()
            if command == "CREATE":
                node.create_session(args[0])
                connection.send(("OK", ""))
            elif command == "CLOSE":
                node.close_session(args[0])
                connection.send(("OK", ""))
            elif command == "STOP":
                node.stop()
                node = None
                connection.send(("OK", ""))
                return
            else:
                raise RuntimeError(f"unknown node command: {command}")
    except BaseException as error:
        _send_error(connection, error)
    finally:
        try:
            if node is not None:
                node.stop()
        finally:
            connection.close()


def _writer_worker(connection: Connection, prefix: str) -> None:
    store = None
    try:
        import sitos

        store = sitos.ParamStore(prefix=prefix, zenoh_config_json=None)
        connection.send(("READY", ""))
        while True:
            command, args = connection.recv()
            if command == "PUT":
                store.put(args[0], args[1], args[2])
                connection.send(("OK", ""))
            elif command == "STOP":
                store.close()
                store = None
                connection.send(("OK", ""))
                return
            else:
                raise RuntimeError(f"unknown writer command: {command}")
    except BaseException as error:
        _send_error(connection, error)
    finally:
        try:
            if store is not None:
                store.close()
        finally:
            connection.close()


def _cache_worker(connection: Connection, prefix: str, sid: str, key: str) -> None:
    cache = None
    attached = False
    try:
        if os.environ.get(FAILURE_ENV) == FAILURE_VALUE:
            connection.send(
                (
                    "TEST_FAILURE",
                    {"pid": os.getpid(), "message": FAILURE_MESSAGE},
                )
            )
            return
        import sitos

        cache = sitos.ParamCache(prefix=prefix, zenoh_config_json=None)
        connection.send(("READY", ""))
        while True:
            command, _ = connection.recv()
            if command == "ATTACH":
                try:
                    cache.attach(sid)
                    attached = True
                    connection.send(("OK", ""))
                except (
                    sitos.NotFoundError,
                    sitos.TimeoutError,
                    sitos.DisconnectedError,
                ):
                    connection.send(("RETRY", ""))
            elif command == "GET":
                connection.send(("VALUE", cache.get(key, default=None)))
            elif command == "DETACH":
                if attached:
                    cache.detach()
                    attached = False
                connection.send(("OK", ""))
            elif command == "STOP":
                if attached:
                    cache.detach()
                    attached = False
                cache.close()
                cache = None
                connection.send(("OK", ""))
                return
            else:
                raise RuntimeError(f"unknown cache command: {command}")
    except BaseException as error:
        _send_error(connection, error)
    finally:
        try:
            if cache is not None:
                if attached:
                    cache.detach()
                cache.close()
        finally:
            connection.close()


def _wait_packet(
    connection: Connection,
    process: multiprocessing.Process,
    label: str,
    deadline: float,
) -> tuple[str, Any]:
    while time.monotonic() < deadline:
        wait = min(0.05, max(0.0, deadline - time.monotonic()))
        try:
            ready = connection.poll(wait)
        except (BrokenPipeError, EOFError, OSError) as error:
            raise WorkerFailure(
                f"{label}: pipe poll failed; exit {process.exitcode}: {error}"
            ) from error
        if ready:
            try:
                status, value = connection.recv()
            except (BrokenPipeError, EOFError, OSError) as error:
                raise WorkerFailure(
                    f"{label}: pipe receive failed; exit {process.exitcode}: {error}"
                ) from error
            if status == "ERROR":
                raise WorkerFailure(f"{label}: {value}")
            return status, value
        if not process.is_alive():
            raise WorkerFailure(f"{label}: exited with {process.exitcode}")
    raise TimeoutError(f"timed out waiting for {label}")


def _wait(
    connection: Connection,
    process: multiprocessing.Process,
    label: str,
    deadline: float,
) -> Any:
    status, value = _wait_packet(connection, process, label, deadline)
    if status in {"RETRY", "MISS"}:
        return None
    if status not in {"READY", "OK", "VALUE"}:
        raise WorkerFailure(f"{label}: unexpected response {status}: {value}")
    return value


def _request(
    connection: Connection,
    process: multiprocessing.Process,
    label: str,
    deadline: float,
    command: str,
    *args: object,
) -> Any:
    try:
        connection.send((command, args))
    except (BrokenPipeError, EOFError, OSError) as error:
        raise WorkerFailure(
            f"{label}: pipe send failed; exit {process.exitcode}: {error}"
        ) from error
    return _wait(connection, process, label, deadline)


def _join_until(process: multiprocessing.Process, deadline: float) -> None:
    while process.is_alive() and time.monotonic() < deadline:
        process.join(min(0.1, max(0.0, deadline - time.monotonic())))


def _stop(
    process: multiprocessing.Process,
    connection: Connection | None,
    label: str,
    deadline: float,
    *,
    require_ack: bool = True,
    allow_forced: bool = False,
) -> None:
    ack_error: BaseException | None = None
    escalation_errors: list[str] = []
    forced = False
    now = time.monotonic()
    graceful_deadline = max(
        now,
        deadline - TERMINATE_RESERVE_SECONDS - KILL_RESERVE_SECONDS,
    )
    terminate_deadline = max(
        graceful_deadline,
        deadline - KILL_RESERVE_SECONDS,
    )
    try:
        if require_ack and not process.is_alive():
            ack_error = WorkerFailure(
                f"{label}: no STOP acknowledgement; exit {process.exitcode}"
            )
        if connection is not None and process.is_alive():
            try:
                connection.send(("STOP", ()))
                status, value = _wait_packet(
                    connection,
                    process,
                    label,
                    graceful_deadline,
                )
                if status != "OK" or value != "":
                    raise WorkerFailure(f"{label}: invalid STOP response {status}: {value}")
            except BaseException as error:
                if require_ack:
                    ack_error = WorkerFailure(
                        f"{label}: STOP handshake failed; exit {process.exitcode}: {error}"
                    )
        _join_until(process, graceful_deadline)
        if process.is_alive():
            forced = True
            try:
                process.terminate()
            except BaseException as error:
                escalation_errors.append(f"terminate failed: {error}")
            _join_until(process, terminate_deadline)
        if process.is_alive() and hasattr(process, "kill"):
            forced = True
            try:
                process.kill()
            except BaseException as error:
                escalation_errors.append(f"kill failed: {error}")
        _join_until(process, deadline)
        if process.is_alive():
            details = (
                f"; {'; '.join(escalation_errors)}" if escalation_errors else ""
            )
            raise WorkerFailure(f"{label}: child survived cleanup{details}")
        if not (allow_forced and forced):
            if ack_error is not None:
                raise ack_error
            if require_ack and process.exitcode != 0:
                raise WorkerFailure(f"{label}: graceful cleanup exit {process.exitcode}")
    finally:
        if connection is not None:
            connection.close()


def _spawn(
    context: multiprocessing.context.BaseContext,
    target: Any,
    args: tuple[Any, ...],
    label: str,
) -> tuple[multiprocessing.Process, Connection]:
    parent, child = context.Pipe()
    process = context.Process(
        target=target,
        args=(child, *args),
        name=f"sitos-example-{label}",
    )
    try:
        process.start()
    except BaseException:
        parent.close()
        child.close()
        raise
    child.close()
    return process, parent


def _cleanup(
    children: list[tuple[str, multiprocessing.Process, Connection]],
    deadline: float,
    *,
    exempt: set[str] | None = None,
    allow_forced: bool = False,
) -> list[str]:
    failures = []
    exempt = exempt or set()
    for label, process, connection in reversed(children):
        child_deadline = min(deadline, time.monotonic() + CHILD_CLEANUP_SECONDS)
        try:
            _stop(
                process,
                connection,
                label,
                child_deadline,
                require_ack=label not in exempt,
                allow_forced=allow_forced,
            )
        except BaseException as error:
            failures.append(f"cleanup {label}: {error}")
    return failures


def _run_failure(
    prefix: str,
    *,
    writer_target: Callable[..., None],
    writer_args_factory: Callable[[str, str], tuple[Any, ...]],
    cache_target: Callable[..., None],
    cache_args_factory: Callable[[str, str], tuple[Any, ...]],
) -> int:
    context = multiprocessing.get_context("spawn")
    children: list[tuple[str, multiprocessing.Process, Connection]] = []
    deadline = time.monotonic() + FAILURE_SECONDS
    sid = f"session_{uuid.uuid4().hex}"
    key = f"failure_{uuid.uuid4().hex}"
    try:
        for label, target, args in (
            ("node", _node_worker, (prefix,)),
            ("writer", writer_target, writer_args_factory(sid, key)),
        ):
            process, connection = _spawn(context, target, args, label)
            children.append((label, process, connection))
            print(f"SITOS_EXAMPLE_TEST_CHILD {label} {process.pid}", file=sys.stderr)
            _wait(connection, process, label, deadline)
        process, connection = _spawn(
            context, cache_target, cache_args_factory(sid, key), "cache"
        )
        children.append(("cache", process, connection))
        print(f"SITOS_EXAMPLE_TEST_CHILD cache {process.pid}", file=sys.stderr)
        status, value = _wait_packet(connection, process, "cache", deadline)
        if status != "TEST_FAILURE":
            raise WorkerFailure("cache failure seam did not produce TEST_FAILURE")
        if value != {"pid": process.pid, "message": FAILURE_MESSAGE}:
            raise WorkerFailure(f"cache failure response mismatch: {value}")
        print(
            f"SITOS_EXAMPLE_TEST_FAILURE cache {value['pid']} {value['message']}",
            file=sys.stderr,
        )
        process.join(1.0)
        if process.is_alive() or process.exitcode != 0:
            raise WorkerFailure("cache failure worker did not exit cleanly")
        failures = _cleanup(
            children,
            time.monotonic() + CLEANUP_SECONDS,
            exempt={"cache"},
            allow_forced=True,
        )
        if failures:
            print("; ".join(failures), file=sys.stderr)
            return 1
        print(FAILURE_CLEANUP_LINE, file=sys.stderr)
        return 70
    except BaseException as error:
        primary = str(error)
        failures = _cleanup(
            children,
            time.monotonic() + CLEANUP_SECONDS,
            exempt={"cache"},
            allow_forced=True,
        )
        print(primary, file=sys.stderr)
        if failures:
            print("; ".join(failures), file=sys.stderr)
        return 1


def run_example(
    prefix: str,
    *,
    writer_target: Callable[..., None],
    writer_args_factory: Callable[[str, str], tuple[Any, ...]],
    cache_target: Callable[..., None],
    cache_args_factory: Callable[[str, str], tuple[Any, ...]],
    put_args_factory: Callable[[str, str, float], tuple[Any, ...]],
    check_command: str,
    observe: Callable[[Any], bool],
    marker: str,
    note: str | None = None,
) -> int:
    failure = os.environ.get(FAILURE_ENV)
    if failure:
        if failure != FAILURE_VALUE:
            print(f"unsupported {FAILURE_ENV} value", file=sys.stderr)
            return 2
        return _run_failure(
            prefix,
            writer_target=writer_target,
            writer_args_factory=writer_args_factory,
            cache_target=cache_target,
            cache_args_factory=cache_args_factory,
        )

    context = multiprocessing.get_context("spawn")
    children: list[tuple[str, multiprocessing.Process, Connection]] = []
    deadline = time.monotonic() + WORK_SECONDS
    primary: BaseException | None = None
    token = f"{os.getpid()}_{uuid.uuid4().hex}"
    sid = f"session_{token}"
    key = f"example_{token}"
    value = 240.0
    try:
        for label, target, args in (
            ("node", _node_worker, (prefix,)),
            ("writer", writer_target, writer_args_factory(sid, key)),
        ):
            process, connection = _spawn(context, target, args, label)
            children.append((label, process, connection))
            _wait(connection, process, label, deadline)
        node_process, node_connection = children[0][1:]
        writer_process, writer_connection = children[1][1:]
        # Create the session only after the node and writer report local READY.
        _request(node_connection, node_process, "node create", deadline, "CREATE", sid)
        cache_process, cache_connection = _spawn(
            context, cache_target, cache_args_factory(sid, key), "cache"
        )
        children.append(("cache", cache_process, cache_connection))
        _wait(cache_connection, cache_process, "cache", deadline)
        # Attach may race default-discovery convergence, so retry it boundedly.
        attached = False
        while time.monotonic() < deadline:
            if _request(
                cache_connection, cache_process, "cache attach", deadline, "ATTACH"
            ) is not None:
                attached = True
                break
        if not attached:
            raise TimeoutError("cache did not attach to the created session")
        # put() is submission-only: resubmit identical data and independently
        # retry cache observation under the same monotonic deadline.
        observed = False
        while time.monotonic() < deadline:
            _request(
                writer_connection,
                writer_process,
                "writer put",
                deadline,
                "PUT",
                *put_args_factory(sid, key, value),
            )
            if observe(
                _request(
                    cache_connection,
                    cache_process,
                    "cache check",
                    deadline,
                    check_command,
                )
            ):
                observed = True
                break
        if not observed:
            raise TimeoutError("cache did not observe the submitted value")
        # Release the cache view before closing the node-owned session.
        _request(cache_connection, cache_process, "cache detach", deadline, "DETACH")
        _request(node_connection, node_process, "node close", deadline, "CLOSE", sid)
    except BaseException as error:
        primary = error
    # STOP performs public close()/stop() before acknowledging; _cleanup then
    # verifies every spawned role is reaped and escalates if graceful exit stalls.
    failures = _cleanup(children, time.monotonic() + CLEANUP_SECONDS)
    if primary is not None or failures:
        print(f"example failed: {primary or '; '.join(failures)}", file=sys.stderr)
        if primary is not None and failures:
            print("cleanup: " + "; ".join(failures), file=sys.stderr)
        return 1
    if note:
        print(note)
    print(marker)
    return 0


def main() -> int:
    prefix = f"sitos/python_example_{os.getpid()}_{uuid.uuid4().hex}"
    return run_example(
        prefix,
        writer_target=_writer_worker,
        writer_args_factory=lambda _sid, _key: (prefix,),
        cache_target=_cache_worker,
        cache_args_factory=lambda sid, key: (prefix, sid, key),
        put_args_factory=lambda sid, key, value: (f"session/{sid}", key, value),
        check_command="GET",
        observe=lambda result: isinstance(result, float)
        and result.hex() == float(240.0).hex(),
        marker="PYTHON_QUICKSTART_OK",
    )


if __name__ == "__main__":
    raise SystemExit(main())
