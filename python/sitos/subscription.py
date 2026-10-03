"""ParamStore subscription callback dispatch (Issue #26, docs/05 §2.1.1 and §3)."""

from __future__ import annotations

import atexit
import logging
import threading
import weakref
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from typing import Any, Literal

__all__ = ["ParamChange", "Subscription"]

_LOGGER = logging.getLogger("sitos")
_LIVE_LOCK = threading.Lock()
# Active subscriptions stay reachable through their dispatcher thread, so a weak set
# tracks exactly the subscriptions whose delivery must stop at interpreter exit.
_LIVE: weakref.WeakSet[Subscription] = weakref.WeakSet()
# Every started dispatcher thread, held until another thread has joined it or it has fully
# terminated, so exit never overlaps a dispatcher's own teardown (DEC-26-007).
_DISPATCHERS: set[threading.Thread] = set()
_SHUTTING_DOWN = threading.Event()


class _DeferredInterrupt:
    """Runs steps to completion, holding a KeyboardInterrupt until all have finished."""

    def __init__(self) -> None:
        self.interrupt: KeyboardInterrupt | None = None

    def run(self, step: Callable[[], object]) -> None:
        while True:
            try:
                step()
                return
            except KeyboardInterrupt as error:
                self.interrupt = self.interrupt or error

    def reraise(self) -> None:
        if self.interrupt is not None:
            raise self.interrupt


def _stop_all(subscriptions: Iterable[Subscription]) -> None:
    """Stop delivery for every subscription even if an interrupt arrives meanwhile.

    Stopping never blocks on a callback, so a KeyboardInterrupt is held until every
    subscription has stopped and is then re-raised; ParamStore.close() joins stay
    interruptible.
    """
    deferred = _DeferredInterrupt()
    for subscription in subscriptions:
        deferred.run(subscription._stop_delivery)
    deferred.reraise()


def _log_exception(message: str) -> None:
    # A raising logging filter or handler must not end dispatch (DEC-26-005).
    try:
        _LOGGER.exception(message)
    except Exception:
        pass


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
        # The thread starts under _LIVE_LOCK, so every tracked dispatcher has fully started:
        # pruning and the exit handler never observe a registered but unstarted thread.
        with _LIVE_LOCK:
            shutting_down = _SHUTTING_DOWN.is_set()
            if not shutting_down:
                # Joining is unnecessary once a started thread has terminated.
                _DISPATCHERS.difference_update(
                    [thread for thread in _DISPATCHERS if not thread.is_alive()]
                )
                try:
                    self._thread.start()
                except BaseException:
                    self._stop_delivery()
                    if self._thread.ident is not None:
                        # Interrupted after the thread began: it must still be joined at exit.
                        _DISPATCHERS.add(self._thread)
                    raise
                _LIVE.add(self)
                _DISPATCHERS.add(self._thread)
        if shutting_down:
            self._stop_delivery()
            raise RuntimeError("cannot subscribe during interpreter shutdown")

    def close(self) -> None:
        """Stop delivery; no callback starts after this returns (DEC-26-004).

        Pending changes are discarded and an in-flight callback is awaited, except when
        called from this subscription's own callback, which returns without waiting.
        """
        self._stop_delivery()
        if threading.current_thread() is not self._thread and self._thread.is_alive():
            self._thread.join()
            with _LIVE_LOCK:
                _DISPATCHERS.discard(self._thread)

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        self.close()
        return False

    def _stop_delivery(self) -> None:
        with self._lock:
            self._closed = True
        # The native close is idempotent; calling it every time keeps a retry after an
        # interrupt between the flag and the close from leaving delivery running.
        self._channel.close()

    def _dispatch(self) -> None:
        try:
            while True:
                try:
                    item = self._channel.next()
                except Exception:
                    # The change was dequeued but could not be converted (for example a STR
                    # value that is not valid UTF-8): report it and keep later changes flowing.
                    _log_exception("sitos subscription change could not be converted; skipped")
                    continue
                if item is None or self._closed:
                    return
                try:
                    self._callback(ParamChange(*item))
                except Exception:
                    # DEC-26-005: report and keep dispatching later changes.
                    _log_exception("sitos subscription callback raised; dispatch continues")
                except BaseException:
                    # SystemExit and similar end this thread; report before closing.
                    _log_exception("sitos subscription callback raised; subscription closed")
                    raise
        finally:
            # A BaseException ends this thread; stop native delivery so nothing queues unread.
            # The thread stays in _DISPATCHERS: only a join proves its teardown finished.
            self._stop_delivery()


def _close_live_subscriptions() -> None:
    """DEC-26-007: close every subscription and join every dispatcher before finalization.

    A KeyboardInterrupt during exit is held until every dispatcher has been joined, so
    exit always reaches quiescence first; a callback that never returns therefore
    blocks exit even across Ctrl-C.
    """
    current = threading.current_thread()
    deferred = _DeferredInterrupt()
    while True:
        with _LIVE_LOCK:
            _SHUTTING_DOWN.set()
            live = list(_LIVE)
            dispatchers = [thread for thread in _DISPATCHERS if thread is not current]
        if not live and not dispatchers:
            break
        # Stop all delivery before any join, so no new callback starts meanwhile.
        for subscription in live:
            deferred.run(subscription._stop_delivery)
        for subscription in live:
            deferred.run(subscription.close)
        for thread in dispatchers:
            deferred.run(thread.join)
        with _LIVE_LOCK:
            _LIVE.difference_update(live)
            _DISPATCHERS.difference_update(dispatchers)
    deferred.reraise()


atexit.register(_close_live_subscriptions)
