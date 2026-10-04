from typing import Literal

import numpy as np
import sitos
from sitos.cache import ParamValue, SitosError as CacheSitosError
from sitos.store import SitosError as StoreSitosError

cache = sitos.ParamCache()
cache.put("array", np.array([1, 2], dtype=np.int16))
array = cache.get_array("array", dtype=np.dtype("<i2"))
value: int = cache.get("array", type=int)
automatic: ParamValue = cache.get("array")
with_default: ParamValue | None = cache.get("missing", None)
typed_with_default: int | str = cache.get("missing", "fallback", type=int)
assert array.ndim == 1
assert isinstance(value, int)

store = sitos.ParamStore()
store.put("base", "value", 1, ack=True, ack_timeout_ms=3000)
store.put_batch("base", [("value", 2)], ack=False, ack_timeout_ms=0)


def accepts_root_error(error: sitos.SitosError) -> None:
    pass


def accepts_cache_error(error: CacheSitosError) -> None:
    accepts_root_error(error)


def accepts_store_error(error: StoreSitosError) -> None:
    accepts_root_error(error)


def _wait_for_local_delivery(cache: sitos.ParamCache) -> None:
    cache.wait_for_local_delivery(timeout_ms=1000)


def _stale(cache: sitos.ParamCache) -> bool:
    stale: bool = cache.stale
    return stale


def _buffer_publisher_surface(publisher: sitos.BufferPublisher) -> None:
    publisher.push("payload", b"bytes")
    applied: sitos.FenceReceipt = publisher.fence(sitos.FenceDurability.APPLIED, timeout=1.0)
    synced: sitos.FenceReceipt = publisher.fence(sitos.FenceDurability.SYNCED, timeout=1.0)
    sequence: int = applied.through_publish_sequence
    durability: sitos.FenceDurability = synced.durability
    assert sequence >= 0
    assert durability in (sitos.FenceDurability.APPLIED, sitos.FenceDurability.SYNCED)


def _subscription_surface(store: sitos.ParamStore) -> None:
    def on_change(change: sitos.ParamChange) -> None:
        kind: Literal["put", "delete"] = change.kind
        key: str = change.key
        value: ParamValue | None = change.value
        assert kind in ("put", "delete") and key and value is not None

    with store.subscribe("base", "recon/", on_change) as subscription:
        subscription.close()
