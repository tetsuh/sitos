"""Python bindings for the sitos parameter store."""

from . import _sitos
from .cache import ParamCache
from .store import (
    DisconnectedError,
    NotFoundError,
    ParamStore,
    ReadOnlyError,
    SitosError,
    TimeoutError,
    TypeMismatchError,
)

__version__ = _sitos.__version__
encode_value = _sitos.encode_value
decode_value = _sitos.decode_value

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
