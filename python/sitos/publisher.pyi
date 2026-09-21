from typing import Any

import numpy.typing as npt

from ._sitos import BufferClass as BufferClass
from ._sitos import FenceDurability as FenceDurability
from ._sitos import FenceReceipt as FenceReceipt

class BufferPublisher:
    def __init__(
        self,
        session_id: str,
        buffer_class: BufferClass,
        *,
        prefix: str = ...,
        zenoh_config_json: str | None = ...,
        query_timeout_ms: int = ...,
    ) -> None: ...
    def push(
        self, key: str, value: bytes | bytearray | memoryview | npt.NDArray[Any]
    ) -> None: ...
    def fence(self, durability: FenceDurability, timeout: float) -> FenceReceipt: ...
