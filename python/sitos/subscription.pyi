from collections.abc import Callable
from dataclasses import dataclass
from typing import Literal

from ._sitos import ParamValue

__all__ = ["ParamChange", "Subscription"]

@dataclass(frozen=True, slots=True)
class ParamChange:
    kind: Literal["put", "delete"]
    key: str
    value: ParamValue | None

class Subscription:
    def __init__(self, channel: object, callback: Callable[[ParamChange], object]) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> Subscription: ...
    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool: ...
