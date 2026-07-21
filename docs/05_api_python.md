# sitos — Python API Specification

Package name: `sitos`. Issue #22 validates CPython 3.12 wheels: the
`manylinux_2_28_x86_64` production target on Ubuntu 24.04/Rocky Linux 10 and
non-publishing `win_amd64` development coverage. Other Python versions and formal Windows
publication are deferred. The package is a nanobind wrapper for the C++ core plus a thin
pythonic layer [P01].

## 1. Value Type Mapping

| payload v1 | Python (Get) | Python (types accepted by Put) |
|---|---|---|
| BOOL | `bool` | `bool` |
| S64 | `int` | `int` (`OverflowError` if outside the 64-bit range) |
| DP | `float` | `float` |
| STR | `str` | `str` |
| BYTES | `bytes` / `numpy.ndarray` (when dtype is specified) | `bytes`, `bytearray`, `memoryview`, C-contiguous `numpy.ndarray` |

* Put of `numpy.ndarray` performs one copy through the buffer protocol (during encoding)
* dtype/shape metadata is not included in the v1 payload ([03] §2.1).
  The reader specifies the dtype

## 2. API

### 2.0 Payload conversion foundation (Issue #22)

The initial Python wheel exposes only the payload-v1 conversion helpers. They are intentionally
small and form the shared foundation for later ParamStore, ParamCache, and SessionView bindings.

```python
import sitos

payload = sitos.encode_value(240.0)  # type tag + little-endian payload-v1 body
value = sitos.decode_value(payload)  # -> 240.0
```

`encode_value` accepts only `bool`, signed-64-bit-range `int`, `float`, `str`, and `bytes`.
Integers outside the signed 64-bit range raise `OverflowError`; unsupported input raises
`TypeError`. `decode_value` accepts only `bytes` and raises `ValueError` for malformed, truncated,
overlong fixed-width, unknown-tag, or invalid UTF-8 payloads. Existing codec behavior is preserved,
including canonical NaN encoding and nonzero BOOL decoding. Buffer-protocol and NumPy conversion
remain Issue #27 scope.

### 2.1 ParamStore

Issue #23 provides a non-callback, ack-less Python facade over the synchronous C++ ParamStore.
Each instance opens and owns its own Transport/session from `zenoh_config_json`; raw session or
Transport sharing is not part of this API. `query_timeout_ms` is a positive integer in the same
milliseconds unit as C++ `ClientConfig`.

```python
import sitos

with sitos.ParamStore(prefix="sitos", zenoh_config_json=None,
                      query_timeout_ms=5000) as store:
    store.put("base", "recon/fov", 240.0)
    store.put_batch("base", [("recon/fov", 240.0), ("recon/kernel", "sharp")])
    store.delete("base", "recon/tmp")

    value = store.get("base", "recon/fov")           # automatic Python type
    value = store.get("base", "recon/fov", type=int) # C05 numeric conversion
    exists = store.contains("base", "recon/fov")
    rows = list(store.list("base", "recon"))         # eager owned snapshot
```

`put_batch` also accepts a Mapping. Pair iterables preserve caller order and duplicate keys;
all entries are validated before one wire submission. Writes are submission-only. `list` eagerly
queries, validates, sorts, and materializes owned `(relative_key, value)` pairs before returning
an iterator.

ParamStore exports `sitos.SitosError` and its `NotFoundError`, `TypeMismatchError`,
`TimeoutError`, `DisconnectedError`, and `ReadOnlyError` subclasses. Missing values raise
`sitos.NotFoundError`, unless an explicit `default` is supplied; the default is returned unchanged
only for NotFound. Type conversion failures raise `sitos.TypeMismatchError`; timeout,
disconnection, and read-only failures raise their corresponding subclasses. `Status::Error` raises
`sitos.SitosError`, while invalid keys and arguments raise built-in `ValueError`. `close` is
idempotent, rejects later calls, and allows already-admitted native operations to finish safely.
Subscriptions and acknowledgements remain outside Issue #23.

### 2.2 ParamCache

ParamCache is session-only under ADR-0022. It has no `attach_base` API. The initial Python binding
in Issue #24 also omits stale/reconnect state, which remains Issue #20 scope.

```python
cache = sitos.ParamCache(prefix="sitos")
cache.attach(sid)                     # Fetch snapshot + overlay

fov  = cache.get("recon/fov", default=240.0)
lut  = cache.get_array("recon/bhc/lut", dtype="float32")   # -> numpy.ndarray
# get_array is zero-copy [P02]: a read-only ndarray pointing to the internal buffer.
# While the ndarray is alive, the underlying buffer is not released (keepalive).
assert lut.flags.writeable is False

cache.put("recon/progress", 0.5)      # Write to overlay + distribute
cache.contains("recon/fov")
dict(cache.items("recon"))            # Enumerate within cache (no communication)
cache.detach(); cache.close()
```

### 2.3 StorageNode / Engines

```python
engine = sitos.InMemoryEngine()
# engine = sitos.RocksDBEngine("/var/lib/myapp/params")  # when the sitos-rocksdb wheel is installed

node = sitos.StorageNode(engine, prefix="sitos")
node.create_session(sid)
node.close_session(sid)
node.active_sessions()                # -> list[str]
node.stop()
```

### 2.4 SessionView

The future Python binding will expose the same read-only semantics as C++ `SessionView` for the
process that owns StorageNode. Python binding work belongs to Issue #25; this section is a plan,
not an implemented API.

```python
view = node.session_view(sid)  # Future Issue #25 binding
fov = view.get("recon/fov", default=240.0)  # Future read-only composite view
keys = view.list("recon/")
```

The view resolves overlay before snapshot and performs no writes. `put`, `put_batch`, and `delete`
are intentionally not part of SessionView. Large binary values use the disk-backed buffers API,
not the session overlay or ParamCache.

Custom engines [X01] can also be defined in Python
(subclass `sitos.StorageEngine` and implement `put/get/list/delete`.
However, state explicitly that performance is the user's responsibility):

```python
class MyEngine(sitos.StorageEngine):
    def put(self, key: str, value: bytes) -> None: ...
    def get(self, key: str) -> bytes | None: ...
    def list(self, prefix: str): ...        # -> Iterable[tuple[str, bytes]]
    def delete(self, key: str) -> None: ...
    # If take_snapshot() is not implemented, use the copy fallback [N03]
```

## 3. GIL and Thread Design [P04]

* zenoh threads in the C++ core do not acquire the GIL
* Notifications to Python callbacks are one-way: “C++-side queue → dedicated Python dispatch
  thread (acquires the GIL)”. zenoh threads never block waiting for the GIL
* `get`/`get_array` use only a C++-side shared lock. They keep holding the GIL
  (because there is no I/O, it is not released)
* In a StorageNode that uses a Python engine (§2.3), zenoh threads call into Python,
  so GIL acquisition occurs. State explicitly that C++ engines are recommended for production use

## 4. Type Stubs and Documentation

* Include type stubs under `sitos/*.pyi` and verify them with mypy/pyright
* docstrings must match the content of the C++ Doxygen comments
* Include an interoperability example in README for “talking to sitos with zenoh-python only” [C03]:

```python
import zenoh, struct
session = zenoh.open(zenoh.Config())
# DP in sitos payload v1 (type tag 2 + double LE)
session.put("sitos/base/recon/fov", bytes([2]) + struct.pack("<d", 240.0),
            encoding=zenoh.Encoding.ZENOH_BYTES.with_schema("sitos.v1"))
```

## 5. Packaging Requirements (Reference)

Wheel structure and build are described in [06_build_test_packaging.md](06_build_test_packaging.md).
The standard wheel installed by `pip install sitos` bundles InMemoryEngine + zenoh-c,
and has only NumPy as an additional runtime dependency (simplified as a required dependency,
not optional `sitos[numpy]`) [P03].
RocksDBEngine is provided as a `sitos-rocksdb` wheel or as a future optional extra.

(END OF DOCUMENT)
