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


def accepts_root_error(error: sitos.SitosError) -> None:
    pass


def accepts_cache_error(error: CacheSitosError) -> None:
    accepts_root_error(error)


def accepts_store_error(error: StoreSitosError) -> None:
    accepts_root_error(error)
