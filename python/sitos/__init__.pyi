from ._sitos import decode_value, encode_value
from .cache import ParamCache
from .store import ParamStore

class SitosError(RuntimeError): ...
class NotFoundError(SitosError): ...
class TypeMismatchError(SitosError): ...
class TimeoutError(SitosError): ...
class DisconnectedError(SitosError): ...
class ReadOnlyError(SitosError): ...

__version__: str

def encode_value(value: bool | int | float | str | bytes | object) -> bytes: ...
def decode_value(payload: bytes) -> bool | int | float | str | bytes: ...

__all__ = [
    "__version__",
    "decode_value",
    "encode_value",
    "ParamCache",
    "ParamStore",
    "SitosError",
    "NotFoundError",
    "TypeMismatchError",
    "TimeoutError",
    "DisconnectedError",
    "ReadOnlyError",
]
