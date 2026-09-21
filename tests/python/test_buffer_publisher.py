"""Python BufferPublisher runtime coverage against a real Zenoh StorageNode fixture."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import numpy as np
import pytest

import sitos


@pytest.fixture
def publisher_fixture() -> tuple[str, str, subprocess.Popen[str]]:
    executable = os.environ.get("SITOS_PYTHON_BUFFER_PUBLISHER_FIXTURE")
    if not executable:
        pytest.fail("SITOS_PYTHON_BUFFER_PUBLISHER_FIXTURE must name the built Zenoh fixture")
    sid = f"publisher_{os.getpid()}"
    prefix = f"sitos/python_publisher_{os.getpid()}"
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
    assert line.startswith("VALUE "), line
    return bytes.fromhex(line.removeprefix("VALUE "))


def test_buffer_publisher_runtime_bytes_numpy_buffer_and_lifetime(publisher_fixture) -> None:
    prefix, sid, process = publisher_fixture
    publisher = sitos.BufferPublisher(sid, sitos.BufferClass.EPHEMERAL, prefix=prefix)
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
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/ephemeral/numpy") == expected
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/ephemeral/bytes") == b"owned"
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/ephemeral/empty") == b""
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/ephemeral/buffer") == b"buffer"

    with pytest.raises(ValueError):
        publisher.fence(sitos.FenceDurability.SYNCED, timeout=2.0)
    with pytest.raises(TypeError):
        publisher.push("str", "unsupported")
    with pytest.raises(ValueError):
        publisher.push("noncontiguous", source[::2])


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
    publisher = sitos.BufferPublisher(sid, sitos.BufferClass.DURABLE, prefix=prefix)
    publisher.push("durable", b"persisted")
    receipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=5.0)
    assert receipt.durability is sitos.FenceDurability.APPLIED
    receipt = publisher.fence(sitos.FenceDurability.SYNCED, timeout=5.0)
    assert receipt.durability is sitos.FenceDurability.SYNCED
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write("recreate\n")
    process.stdin.flush()
    assert process.stdout.readline().strip() == "RECREATED"
    replacement = sitos.BufferPublisher(sid, sitos.BufferClass.DURABLE, prefix=prefix)
    assert _read_fixture(process, f"{prefix}/buffers/{sid}/durable/durable") == b"persisted"
    del replacement


def test_buffer_publisher_enum_surface() -> None:
    assert sitos.BufferClass.DURABLE is not sitos.BufferClass.EPHEMERAL
    assert sitos.FenceDurability.APPLIED is not sitos.FenceDurability.SYNCED
    assert hasattr(sitos, "BufferPublisher")
    assert hasattr(sitos, "FenceReceipt")
