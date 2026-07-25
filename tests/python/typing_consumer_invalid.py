import sitos

cache = sitos.ParamCache()
cache.get_array("array")  # E: missing keyword-only dtype
cache.put("array", bytearray(b"invalid"))  # E: unsupported ParamInput
cache.get("array", type=list)  # E: unsupported exact built-in type
sitos.encode_value(bytearray(b"invalid"))  # E: unsupported encode input
wrong_default: int = cache.get("missing", "fallback", type=int)  # E: default remains possible
node: sitos.StorageNode
node.session_view()  # E: missing session ID
view: sitos.SessionView
view.get("missing", None)  # E: default is keyword-only
