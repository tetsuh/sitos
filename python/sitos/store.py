"""ParamStore public Python facade."""

from . import _sitos

ParamStore = _sitos.ParamStore
SitosError = _sitos.SitosError
NotFoundError = _sitos.NotFoundError
TypeMismatchError = _sitos.TypeMismatchError
TimeoutError = _sitos.TimeoutError
DisconnectedError = _sitos.DisconnectedError
ReadOnlyError = _sitos.ReadOnlyError

__all__ = [
    "ParamStore",
    "SitosError",
    "NotFoundError",
    "TypeMismatchError",
    "TimeoutError",
    "DisconnectedError",
    "ReadOnlyError",
]
