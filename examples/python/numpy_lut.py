#!/usr/bin/env python3
"""Process-isolated NumPy fixed-width LUT and read-only cache-view example.

The sibling quickstart module supplies only private process orchestration helpers;
this example keeps its own NumPy writer and cache semantics.
"""
from __future__ import annotations

import os
import sys
import uuid
from multiprocessing.connection import Connection

import quickstart as _quickstart

_VALUES = (1.5, -2.25, 3.75, 0.5)
_GOLDEN_BYTES = bytes.fromhex("0000c03f000010c0000070400000003f")


def _writer_worker(
    connection: Connection, prefix: str, sid: str, key: str
) -> None:
    store = None
    try:
        import numpy as np
        import sitos

        values = np.asarray(_VALUES, dtype=np.dtype("<f4"))
        if values.tobytes() != _GOLDEN_BYTES:
            raise AssertionError("fixed LUT does not match its little-endian golden bytes")
        store = sitos.ParamStore(prefix=prefix, zenoh_config_json=None)
        connection.send(("READY", ""))
        while True:
            command, _ = connection.recv()
            if command == "PUT":
                store.put(f"session/{sid}", key, values)
                connection.send(("OK", ""))
            elif command == "STOP":
                store.close()
                store = None
                connection.send(("OK", ""))
                return
            else:
                raise RuntimeError(f"unknown writer command: {command}")
    except BaseException as error:
        _quickstart._send_error(connection, error)
    finally:
        try:
            if store is not None:
                store.close()
        finally:
            connection.close()


def _cache_worker(
    connection: Connection, prefix: str, sid: str, key: str
) -> None:
    cache = None
    attached = False
    try:
        if os.environ.get(_quickstart.FAILURE_ENV) == _quickstart.FAILURE_VALUE:
            connection.send(
                (
                    "TEST_FAILURE",
                    {
                        "pid": os.getpid(),
                        "message": _quickstart.FAILURE_MESSAGE,
                    },
                )
            )
            return
        import numpy as np
        import sitos

        expected = np.asarray(_VALUES, dtype=np.dtype("<f4"))
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
            elif command == "CHECK":
                try:
                    first = cache.get_array(key, dtype=np.dtype("<f4"))
                except sitos.NotFoundError:
                    connection.send(("MISS", ""))
                    continue
                second = cache.get_array(key, dtype=np.dtype("<f4"))
                if first.ndim != 1 or first.shape != (4,):
                    raise AssertionError(f"unexpected LUT shape: {first.shape}")
                if first.dtype != np.dtype("<f4") or second.dtype != np.dtype("<f4"):
                    raise AssertionError("caller-supplied dtype was not preserved")
                if first.tobytes() != _GOLDEN_BYTES:
                    raise AssertionError("first cached view differs from golden bytes")
                if second.tobytes() != _GOLDEN_BYTES:
                    raise AssertionError("second cached view differs from golden bytes")
                if first.tobytes() != expected.tobytes():
                    raise AssertionError("cached bytes differ from fixed LUT values")
                if first.flags.writeable or second.flags.writeable:
                    raise AssertionError("cached views must be read-only")
                if first.__array_interface__["data"][0] != second.__array_interface__["data"][0]:
                    raise AssertionError("repeated views do not share their data pointer")
                if not np.shares_memory(first, second):
                    raise AssertionError("repeated views do not share memory")
                try:
                    first[0] = 99.0
                except ValueError:
                    pass
                else:
                    raise AssertionError("read-only view accepted assignment")
                connection.send(("OK", "checked"))
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
        _quickstart._send_error(connection, error)
    finally:
        try:
            if cache is not None:
                if attached:
                    cache.detach()
                cache.close()
        finally:
            connection.close()


def main() -> int:
    prefix = f"sitos/python_numpy_{os.getpid()}_{uuid.uuid4().hex}"
    return _quickstart.run_example(
        prefix,
        writer_target=_writer_worker,
        writer_args_factory=lambda sid, key: (prefix, sid, key),
        cache_target=_cache_worker,
        cache_args_factory=lambda sid, key: (prefix, sid, key),
        put_args_factory=lambda _sid, _key, _value: (),
        check_command="CHECK",
        observe=lambda result: result == "checked",
        marker="PYTHON_NUMPY_LUT_OK",
        note=(
            "NumPy note: shape and dtype metadata are not transported; "
            "dtype was supplied by caller."
        ),
    )


if __name__ == "__main__":
    raise SystemExit(main())
