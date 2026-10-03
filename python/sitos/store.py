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
from .subscription import ParamChange, Subscription, _stop_all


class ParamStore(_sitos.ParamStore):
    """Synchronous ParamStore client with callback subscriptions (DEC-26-001)."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        super().__init__(*args, **kwargs)
        self._subscriptions_lock = threading.Lock()
        self._subscriptions: weakref.WeakSet[Subscription] = weakref.WeakSet()
        self._closing = False
        self._close_done = threading.Event()
        self._close_dispatchers: frozenset[threading.Thread] = frozenset()

    def subscribe(
        self, scope: str, prefix: str, callback: Callable[[ParamChange], object]
    ) -> Subscription:
        """Deliver matching changes to `callback` on a dedicated dispatcher thread."""
        if not callable(callback):
            raise TypeError("subscription callback must be callable")
        subscription = Subscription(self._subscribe(scope, prefix), callback)
        with self._subscriptions_lock:
            closing = self._closing
            if not closing:
                # Register, then start under the lock: a concurrent close() either sees this
                # subscription or prevents it from ever starting, and an interrupt after
                # the start cannot leave it unreachable from close().
                self._subscriptions.add(subscription)
                try:
                    subscription._start()
                except BaseException:
                    self._subscriptions.discard(subscription)
                    raise
        if closing:
            subscription.close()
            raise ValueError("ParamStore is closed")
        return subscription

    def close(self) -> None:
        """Close every subscription of this store (DEC-26-004), then the store.

        A concurrent second call waits for the first to finish, except from one of this
        store's dispatcher threads, which the first call is joining.
        """
        with self._subscriptions_lock:
            first = not self._closing
            if first:
                self._closing = True
                subscriptions = list(self._subscriptions)
                self._subscriptions.clear()
                self._close_dispatchers = frozenset(
                    subscription._thread for subscription in subscriptions
                )
        if not first:
            if threading.current_thread() not in self._close_dispatchers:
                self._close_done.wait()
            return
        try:
            # Stop all delivery before any join, so an interrupted join cannot leave a
            # subscription delivering behind a store that reports itself closed.
            _stop_all(subscriptions)
            for subscription in subscriptions:
                subscription.close()
        finally:
            try:
                super().close()
            finally:
                self._close_done.set()

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
