import sitos

cache = sitos.ParamCache()
cache.get_array("array")
cache.put("array", bytearray(b"invalid"))
cache.get("array", type=list)
