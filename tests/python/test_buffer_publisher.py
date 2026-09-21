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


def test_buffer_publisher_runtime_bytes_numpy_buffer_and_lifetime(publisher_fixture) -> None:
    prefix, sid, _ = publisher_fixture
    publisher = sitos.BufferPublisher(sid, sitos.BufferClass.EPHEMERAL, prefix=prefix)
    source = np.arange(8, dtype=np.int16)
    expected = source.tobytes()
    publisher.push("numpy", source)
    source[:] = -1
    publisher.push("bytes", bytes(b"owned"))
    mutable = bytearray(b"buffer")
    publisher.push("buffer", memoryview(mutable))
    mutable[:] = b"mutate"
    receipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
    assert receipt.durability is sitos.FenceDurability.APPLIED
    assert receipt.through_publish_sequence == 3
    assert expected != source.tobytes()

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
    prefix, sid, _ = publisher_fixture
    publisher = sitos.BufferPublisher(sid, sitos.BufferClass.DURABLE, prefix=prefix)
    publisher.push("durable", b"persisted")
    receipt = publisher.fence(sitos.FenceDurability.SYNCED, timeout=5.0)
    assert receipt.durability is sitos.FenceDurability.SYNCED


def test_buffer_publisher_enum_surface() -> None:
    assert sitos.BufferClass.DURABLE is not sitos.BufferClass.EPHEMERAL
    assert sitos.FenceDurability.APPLIED is not sitos.FenceDurability.SYNCED
    assert hasattr(sitos, "BufferPublisher")
    assert hasattr(sitos, "FenceReceipt")
