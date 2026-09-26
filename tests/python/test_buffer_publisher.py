"""Python BufferPublisher runtime coverage against a real Zenoh StorageNode fixture."""

from __future__ import annotations

import math
import os
import shutil
import subprocess
import tempfile
import time
import numpy as np
import pytest

import sitos


@pytest.fixture
def publisher_fixture() -> tuple[str, str, subprocess.Popen[str]]:
    executable = os.environ.get("SITOS_PYTHON_BUFFER_PUBLISHER_FIXTURE")
    if not executable:
        pytest.fail("SITOS_PYTHON_BUFFER_PUBLISHER_FIXTURE must name the built Zenoh fixture")
    nonce = f"{os.getpid()}_{time.time_ns()}"
    sid = f"publisher_{nonce}"
    prefix = f"sitos/python_publisher_{nonce}"
    rocks_root = tempfile.mkdtemp(prefix="sitos-python-publisher-")
    args = [executable, prefix, sid]
    if os.environ.get("SITOS_PYTHON_PUBLISHER_ROCKSDB") == "1":
        args.append(rocks_root)
    process = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdout is not None
    assert process.stdout.readline().strip() == "READY"
    try:
        yield prefix, sid, process
    finally:
        if process.stdin is not None:
            process.stdin.write("stop\n")
            process.stdin.flush()
        process.wait(timeout=10)
        shutil.rmtree(rocks_root, ignore_errors=True)


def _read_fixture(process: subprocess.Popen[str], key: str) -> bytes:
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write(f"read {key}\n")
    process.stdin.flush()
    line = process.stdout.readline().strip()
    assert line == "VALUE" or line.startswith("VALUE "), line
    return bytes.fromhex(line.removeprefix("VALUE").strip())


def _open_fixture_publisher(
    sid: str, buffer_class: sitos.BufferClass, prefix: str
) -> sitos.BufferPublisher:
    deadline = time.monotonic() + 5.0
    while True:
        try:
            return sitos.BufferPublisher(
                sid, buffer_class, prefix=prefix, query_timeout_ms=500
            )
        except sitos.NotFoundError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def test_buffer_publisher_runtime_bytes_numpy_buffer_and_lifetime(publisher_fixture) -> None:
    prefix, sid, process = publisher_fixture
    publisher = _open_fixture_publisher(sid, sitos.BufferClass.DURABLE, prefix)
    source = np.arange(8, dtype=np.int16)
    expected = source.tobytes()
    publisher.push("numpy", source)
    source[:] = -1
    publisher.push("bytes", bytes(b"owned"))
    publisher.push("empty", memoryview(b""))
    mutable = bytearray(b"buffer")
    publisher.push("buffer", memoryview(mutable))
    mutable[:] = b"mutate"
    receipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
    assert receipt.durability is sitos.FenceDurability.APPLIED
    assert receipt.through_publish_sequence == 4
    assert expected != source.tobytes()
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/numpy") == expected
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/bytes") == b"owned"
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/empty") == b""
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/buffer") == b"buffer"

    ephemeral = _open_fixture_publisher(sid, sitos.BufferClass.EPHEMERAL, prefix)
    with pytest.raises(ValueError):
        ephemeral.fence(sitos.FenceDurability.SYNCED, timeout=2.0)
    tiny_timeout = ephemeral
    try:
        tiny_receipt = tiny_timeout.fence(sitos.FenceDurability.APPLIED, timeout=0.0001)
        assert tiny_receipt.durability is sitos.FenceDurability.APPLIED
    except sitos.TimeoutError:
        pass
    if os.environ.get("SITOS_PYTHON_PUBLISHER_ROCKSDB") == "1":
        synced = publisher.fence(sitos.FenceDurability.SYNCED, timeout=2.0)
        assert synced.durability is sitos.FenceDurability.SYNCED
        publisher.push("after-sync-success", b"usable")
    else:
        with pytest.raises(ValueError):
            publisher.fence(sitos.FenceDurability.SYNCED, timeout=2.0)
        with pytest.raises(sitos.DisconnectedError):
            publisher.push("after-sync-error", b"disconnected")
    with pytest.raises(TypeError):
        publisher.push("str", "unsupported")
    with pytest.raises(ValueError):
        publisher.push("noncontiguous", source[::2])


def test_buffer_publisher_recreate_old_fence_timeout_then_disconnects(publisher_fixture) -> None:
    prefix, sid, process = publisher_fixture
    publisher = _open_fixture_publisher(sid, sitos.BufferClass.DURABLE, prefix)
    publisher.push("before", b"before")
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write("recreate\n")
    process.stdin.flush()
    assert process.stdout.readline().strip() == "RECREATED"
    with pytest.raises(sitos.TimeoutError):
        publisher.fence(sitos.FenceDurability.APPLIED, timeout=0.1)
    with pytest.raises(sitos.DisconnectedError):
        publisher.push("later", b"later")
    with pytest.raises(TypeError):
        publisher.fence(sitos.FenceDurability.APPLIED, timeout=True)


def test_buffer_publisher_fence_rejects_int64_millisecond_boundary(publisher_fixture) -> None:
    prefix, sid, _ = publisher_fixture
    publisher = _open_fixture_publisher(sid, sitos.BufferClass.DURABLE, prefix)
    timeout = float(2**63) / 1000.0
    assert math.ceil(timeout * 1000.0) == 2**63
    with pytest.raises(ValueError, match="timeout is outside the C\\+\\+ duration range"):
        publisher.fence(sitos.FenceDurability.APPLIED, timeout=timeout)


def test_buffer_publisher_missing_session_maps_not_found() -> None:
    with pytest.raises(sitos.NotFoundError):
        sitos.BufferPublisher(
            f"missing_{os.getpid()}",
            sitos.BufferClass.DURABLE,
            prefix=f"sitos/python_missing_{os.getpid()}",
            query_timeout_ms=500,
        )


def test_buffer_publisher_rocksdb_synced_runtime(publisher_fixture) -> None:
    if os.environ.get("SITOS_PYTHON_PUBLISHER_ROCKSDB") != "1":
        pytest.skip("RocksDB runtime lane is provisioned separately")
    prefix, sid, process = publisher_fixture
    publisher = _open_fixture_publisher(sid, sitos.BufferClass.DURABLE, prefix)
    publisher.push("durable", b"persisted")
    receipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=5.0)
    assert receipt.durability is sitos.FenceDurability.APPLIED
    receipt = publisher.fence(sitos.FenceDurability.SYNCED, timeout=5.0)
    assert receipt.durability is sitos.FenceDurability.SYNCED
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write("recreate\n")
    process.stdin.flush()
    assert process.stdout.readline().strip() == "RECREATED"
    replacement = _open_fixture_publisher(sid, sitos.BufferClass.DURABLE, prefix)
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/durable") == b"persisted"
    del replacement


def test_buffer_publisher_enum_surface() -> None:
    assert sitos.BufferClass.DURABLE is not sitos.BufferClass.EPHEMERAL
    assert sitos.FenceDurability.APPLIED is not sitos.FenceDurability.SYNCED
    assert hasattr(sitos, "BufferPublisher")
    assert hasattr(sitos, "FenceReceipt")
