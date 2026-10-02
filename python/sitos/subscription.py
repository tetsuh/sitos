"""ParamStore subscription callback dispatch (Issue #26, docs/05 §2.1 and §3)."""

from __future__ import annotations

import atexit
import logging
import threading
import weakref
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Literal

__all__ = ["ParamChange", "Subscription"]

_LOGGER = logging.getLogger("sitos")
_LIVE_LOCK = threading.Lock()
# Active subscriptions stay reachable through their dispatcher thread, so a weak set
# tracks exactly the subscriptions that still need closing at interpreter exit.
_LIVE: weakref.WeakSet[Subscription] = weakref.WeakSet()
_SHUTTING_DOWN = threading.Event()


@dataclass(frozen=True, slots=True)
class ParamChange:
    """One relative-key change delivered to a subscription callback (DEC-26-001)."""

    kind: Literal["put", "delete"]
    key: str
    value: Any


class Subscription:
    """A ParamStore subscription served by one daemon dispatcher thread (DEC-26-002).

    Native delivery only appends to an unbounded queue (DEC-26-003); this thread drains it
    holding the GIL and invokes the callback serially. ParamStore.subscribe creates and
    starts subscriptions; they are not constructed directly.
    """

    __slots__ = ("__weakref__", "_callback", "_channel", "_closed", "_lock", "_thread")

    def __init__(self, channel: Any, callback: Callable[[ParamChange], object]) -> None:
        self._channel = channel
        self._callback = callback
        self._closed = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(
            target=self._dispatch, name="sitos-subscription", daemon=True
        )

    def _start(self) -> None:
        # The subscription stays in _LIVE until its dispatcher thread has exited, so the
        # atexit handler joins every dispatcher before interpreter finalization (DEC-26-007).
        with _LIVE_LOCK:
            shutting_down = _SHUTTING_DOWN.is_set()
            if not shutting_down:
                _LIVE.add(self)
        if shutting_down:
            self._stop_delivery()
            raise RuntimeError("cannot subscribe during interpreter shutdown")
        try:
            self._thread.start()
        except BaseException:
            self._stop_delivery()
            with _LIVE_LOCK:
                _LIVE.discard(self)
            raise

    def close(self) -> None:
        """Stop delivery; no callback starts after this returns (DEC-26-004).

        Pending changes are discarded and an in-flight callback is awaited, except when
        called from this subscription's own callback, which returns without waiting.
        """
        self._stop_delivery()
        if threading.current_thread() is not self._thread and self._thread.ident is not None:
            self._thread.join()

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        self.close()
        return False

    def _stop_delivery(self) -> None:
        with self._lock:
            first = not self._closed
            self._closed = True
        if first:
            self._channel.close()

    def _dispatch(self) -> None:
        try:
            while True:
                try:
                    item = self._channel.next()
                except Exception:
                    _LOGGER.exception("sitos subscription dispatch failed; subscription closed")
                    return
                if item is None or self._closed:
                    return
                try:
                    self._callback(ParamChange(*item))
                except Exception:
                    # DEC-26-005: report and keep dispatching later changes.
                    _LOGGER.exception("sitos subscription callback raised; dispatch continues")
        finally:
            # A BaseException ends this thread; stop native delivery so nothing queues unread.
            self._stop_delivery()
            with _LIVE_LOCK:
                _LIVE.discard(self)


def _close_live_subscriptions() -> None:
    """DEC-26-007: close and join every dispatcher before interpreter finalization."""
    while True:
        with _LIVE_LOCK:
            _SHUTTING_DOWN.set()
            live = list(_LIVE)
        if not live:
            return
        for subscription in live:
            subscription.close()


atexit.register(_close_live_subscriptions)
