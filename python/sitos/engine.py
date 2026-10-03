"""Custom storage engines written in Python (Issue #28, docs/05 §2.4.1)."""

from __future__ import annotations

from collections.abc import Iterable

__all__ = ["StorageEngine", "StorageReader"]


class StorageReader:
    """Read-only view returned by an engine's optional `take_snapshot()` (DEC-28-004).

    Subclasses implement `get` and `list` with the same contract as `StorageEngine`.
    """

    def get(self, key: str) -> bytes | None:
        """Return the stored bytes for `key`, or None when it is absent."""
        raise NotImplementedError

    def list(self, prefix: str) -> Iterable[tuple[str, bytes]]:
        """Return `(key, value)` pairs whose key starts with `prefix`, in any order.

        sitos materializes the result, rejects malformed pairs, keys outside `prefix`,
        and duplicate keys, and delivers the entries in ascending key order.
        """
        raise NotImplementedError


class StorageEngine(StorageReader):
    """Base class for a StorageNode persistence backend written in Python (DEC-28-001).

    Implement `get`, `list`, `put`, and `delete`; keys are relative strings and values are
    opaque payload bytes. `put` and `delete` fail only by returning False or raising.
    Optionally implement a callable `take_snapshot()` returning a `StorageReader`; without
    it, sitos copies every entry through `list` when a session is created (N03).

    StorageNode calls these methods from zenoh threads, and every call acquires the Python
    GIL (DEC-28-006). Calls may arrive concurrently, so protect shared state with a lock.
    Python engines suit prototypes and tests; prefer a C++ StorageEngine for production
    throughput. A raised exception is logged on the "sitos" logger and treated as an engine
    failure (DEC-28-002). Python engines report no synchronization capability and offer no
    synchronization barrier (DEC-28-005).
    """

    def put(self, key: str, value: bytes) -> bool | None:
        """Store `value` under `key`; return False to report failure."""
        raise NotImplementedError

    def delete(self, key: str) -> bool | None:
        """Remove `key`; deleting an absent key succeeds. Return False to report failure."""
        raise NotImplementedError
