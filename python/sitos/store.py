"""ParamStore is provided by the nanobind extension; subscriptions add Python dispatch."""

from __future__ import annotations

import threading
import weakref
from collections.abc import Callable

from . import _sitos
from ._sitos import (
    CatalogUnavailableError,
    DisconnectedError,
    NotFoundError,
    OutcomeUnknownError,
    ReadOnlyError,
    SitosError,
    StateLostError,
    TimeoutError,
    TypeMismatchError,
)
from .subscription import ParamChange, Subscription


class ParamStore(_sitos.ParamStore):
    """Synchronous ParamStore client with callback subscriptions (DEC-26-001)."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        super().__init__(*args, **kwargs)
        self._subscriptions_lock = threading.Lock()
        self._subscriptions: weakref.WeakSet[Subscription] = weakref.WeakSet()
        self._closing = False

    def subscribe(
        self, scope: str, prefix: str, callback: Callable[[ParamChange], object]
    ) -> Subscription:
        """Deliver matching changes to `callback` on a dedicated dispatcher thread."""
        if not callable(callback):
            raise TypeError("subscription callback must be callable")
        subscription = Subscription(self._subscribe(scope, prefix), callback)
        with self._subscriptions_lock:
            if self._closing:
                subscription.close()
                raise ValueError("ParamStore is closed")
            subscription._start()
            self._subscriptions.add(subscription)
        return subscription

    def close(self) -> None:
        """Close every subscription of this store (DEC-26-004), then the store."""
        with self._subscriptions_lock:
            self._closing = True
            subscriptions = list(self._subscriptions)
            self._subscriptions.clear()
        for subscription in subscriptions:
            subscription.close()
        super().close()

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        self.close()
        return False


__all__ = [
    "ParamStore",
    "ParamChange",
    "Subscription",
    "SitosError",
    "NotFoundError",
    "TypeMismatchError",
    "TimeoutError",
    "DisconnectedError",
    "ReadOnlyError",
    "OutcomeUnknownError",
    "StateLostError",
    "CatalogUnavailableError",
]
