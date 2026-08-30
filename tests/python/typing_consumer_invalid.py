import sitos

cache = sitos.ParamCache()
cache.wait_for_local_delivery(1000)  # E: wait timeout_ms is keyword-only
cache.wait_for_local_delivery()  # E: missing wait timeout_ms
cache.wait_for_local_delivery(timeout_ms="1000")  # E: wait timeout_ms must be int
cache.get_array("array")  # E: missing keyword-only dtype
cache.put("array", bytearray(b"invalid"))  # E: unsupported ParamInput
cache.get("array", type=list)  # E: unsupported exact built-in type
sitos.encode_value(bytearray(b"invalid"))  # E: unsupported encode input
wrong_default: int = cache.get("missing", "fallback", type=int)  # E: default remains possible
node: sitos.StorageNode
node.session_view()  # E: missing session ID
view: sitos.SessionView
view.get("missing", None)  # E: default is keyword-only

store = sitos.ParamStore()
store.put("base", "key", 1, False)  # E: acknowledgement options are keyword-only
store.put("base", "key", 1, ack=1)  # E: ack must be bool
store.put_batch("base", [], True)  # E: acknowledgement options are keyword-only
