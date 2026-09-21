"""BufferPublisher API and input-contract coverage."""

from __future__ import annotations

import os

import numpy as np
import pytest

import sitos


@pytest.mark.skipif(
    os.environ.get("SITOS_PYTHON_PUBLISHER_LIVE") != "1",
    reason="requires the serial Zenoh-enabled publisher lane",
)
def test_buffer_publisher_bytes_numpy_buffer_and_fence() -> None:
    sid = f"publisher_{os.getpid()}"
    prefix = f"sitos/python_publisher_{os.getpid()}"
    # The node process is supplied by the serial live lane. Ephemeral publication
    # still qualifies owned payload, NumPy contiguity, and applied receipt parity.
    publisher = sitos.BufferPublisher(sid, sitos.BufferClass.EPHEMERAL, prefix=prefix)
    try:
        publisher.push("bytes", b"owned")
        source = np.arange(8, dtype=np.uint8)
        publisher.push("numpy", source)
        publisher.push("buffer", memoryview(bytearray(b"buffer")))
        receipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
        assert receipt.durability is sitos.FenceDurability.APPLIED
        assert receipt.through_publish_sequence == 3
        with pytest.raises(ValueError):
            publisher.fence(sitos.FenceDurability.SYNCED, timeout=2.0)
        with pytest.raises(TypeError):
            publisher.push("str", "unsupported")
        with pytest.raises(TypeError):
            publisher.push("noncontiguous", source[::2])
    finally:
        del publisher


def test_buffer_publisher_enum_surface() -> None:
    assert sitos.BufferClass.DURABLE is not sitos.BufferClass.EPHEMERAL
    assert sitos.FenceDurability.APPLIED is not sitos.FenceDurability.SYNCED
