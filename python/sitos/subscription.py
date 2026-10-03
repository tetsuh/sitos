"""ParamStore subscription callback dispatch (Issue #26, docs/05 §2.1.1 and §3)."""

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
_LOCK = threading.Lock()
# Subscriptions to close and dispatcher threads to join at interpreter exit (DEC-26-007).
_LIVE: weakref.WeakSet[Subscription] = weakref.WeakSet()
_DISPATCHERS: set[threading.Thread] = set()
_SHUTTING_DOWN = False


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

    __slots__ = ("__weakref__", "_callback", "_channel", "_closed", "_thread")

    def __init__(self, channel: Any, callback: Callable[[ParamChange], object]) -> None:
        self._channel = channel
        self._callback = callback
        self._closed = False
        self._thread = threading.Thread(
            target=self._dispatch, name="sitos-subscription", daemon=True
        )

    def _start(self) -> None:
        with _LOCK:
            if _SHUTTING_DOWN:
                self._channel.close()
                raise RuntimeError("cannot subscribe during interpreter shutdown")
            _DISPATCHERS.difference_update([t for t in _DISPATCHERS if not t.is_alive()])
            self._thread.start()
            _LIVE.add(self)
            _DISPATCHERS.add(self._thread)

    def close(self) -> None:
        """Stop delivery; no callback starts after this returns (DEC-26-004).

        Pending changes are discarded and an in-flight callback is awaited, except when
        called from this subscription's own callback, which returns without waiting.
        """
        self._closed = True
        self._channel.close()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join()

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        self.close()
        return False

    def _dispatch(self) -> None:
        try:
            while not self._closed:
                try:
                    item = self._channel.next()
                    if item is None or self._closed:
                        return
                    self._callback(ParamChange(*item))
                except Exception:
                    # DEC-26-005: a raising callback or an unconvertible change is reported
                    # and skipped; later changes keep flowing.
                    _LOGGER.exception("sitos subscription callback failed; dispatch continues")
        finally:
            self._closed = True
            self._channel.close()


def _close_live_subscriptions() -> None:
    """DEC-26-007: close every subscription and join every dispatcher before finalization."""
    global _SHUTTING_DOWN
    with _LOCK:
        _SHUTTING_DOWN = True
        live = list(_LIVE)
        dispatchers = list(_DISPATCHERS)
    for subscription in live:
        subscription.close()
    current = threading.current_thread()
    for thread in dispatchers:
        if thread is not current:
            thread.join()


atexit.register(_close_live_subscriptions)
