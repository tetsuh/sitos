import numpy as np
import sitos

cache = sitos.ParamCache()
cache.put("array", np.array([1, 2], dtype=np.int16))
array = cache.get_array("array", dtype=np.dtype("<i2"))
value: int = cache.get("array", type=int)
assert array.ndim == 1
assert isinstance(value, int)
