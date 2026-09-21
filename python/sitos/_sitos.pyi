from enum import Enum
from typing import Any, TypeAlias

import numpy.typing as npt

ParamValue: TypeAlias = bool | int | float | str | bytes
ParamInput: TypeAlias = ParamValue | npt.NDArray[Any]

class SitosError(RuntimeError): ...
class NotFoundError(SitosError): ...
class TypeMismatchError(SitosError): ...
class TimeoutError(SitosError): ...
class DisconnectedError(SitosError): ...
class ReadOnlyError(SitosError): ...
class OutcomeUnknownError(SitosError): ...

class BufferClass(Enum):
    DURABLE: BufferClass
    EPHEMERAL: BufferClass

class FenceDurability(Enum):
    APPLIED: FenceDurability
    SYNCED: FenceDurability

class FenceReceipt:
    through_publish_sequence: int
    durability: FenceDurability

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

__version__: str

def encode_value(value: ParamInput) -> bytes: ...
def decode_value(payload: bytes) -> ParamValue: ...
