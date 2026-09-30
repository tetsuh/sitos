"""ParamCache is provided by the nanobind extension."""

from ._sitos import (
    CatalogUnavailableError,
    DisconnectedError,
    NotFoundError,
    OutcomeUnknownError,
    ParamCache,
    ReadOnlyError,
    SitosError,
    StateLostError,
    TimeoutError,  # noqa: A004 - re-export the public sitos exception.
    TypeMismatchError,
)

__all__ = [
    "DisconnectedError",
    "NotFoundError",
    "ParamCache",
    "ReadOnlyError",
    "OutcomeUnknownError",
    "StateLostError",
    "CatalogUnavailableError",
    "SitosError",
    "TimeoutError",
    "TypeMismatchError",
]
