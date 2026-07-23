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
| BYTES | `bytes` | `bytes` or supported NumPy arrays |

Issue #27 adds exact NumPy ndarray conversion and zero-copy `ParamCache.get_array`. NumPy 2.x is
required; NumPy 1.x is not supported. NumPy inputs must be C-contiguous, object-free, fixed-width
numeric or boolean arrays and are copied once into owned BYTES; shape and dtype are not serialized.
General buffer objects, `bytearray`, and `memoryview` remain rejected. Payload v1 does not carry
dtype or shape metadata ([03] §2.1).

## 2. API

### 2.0 Payload conversion foundation (Issue #22)

The initial Python wheel exposes only the payload-v1 conversion helpers. They are intentionally
small and form the shared foundation for later ParamStore, ParamCache, and SessionView bindings.

```python
import sitos

payload = sitos.encode_value(240.0)  # type tag + little-endian payload-v1 body
value = sitos.decode_value(payload)  # -> 240.0
```

`encode_value` accepts `bool`, signed-64-bit-range `int`, `float`, `str`, `bytes`, and exact NumPy
ndarrays with supported C-contiguous, object-free, fixed-width numeric or boolean dtypes. Array
input is copied once into owned BYTES; shape and dtype are not serialized. Integers outside the
signed 64-bit range raise `OverflowError`; unsupported input raises `TypeError`. `decode_value`
accepts only `bytes` and raises `ValueError` for malformed, truncated, overlong fixed-width,
unknown-tag, or
invalid UTF-8 payloads. Existing codec behavior is preserved, including canonical NaN encoding and
nonzero BOOL decoding. General buffer-protocol inputs remain unsupported.

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

Issue #24 provides a non-callback, session-only Python facade over C++ ParamCache under ADR-0022 and
ADR-0023. Each instance opens and owns its Transport/session from `zenoh_config_json`; it has no
`attach_base` or raw Transport-injection API. `query_timeout_ms` is a positive integer used by the
initial snapshot and overlay fetches.

```python
with sitos.ParamCache(prefix="sitos", zenoh_config_json=None,
                      query_timeout_ms=5000) as cache:
    cache.attach(sid)                              # Fetch snapshot + overlay
    fov = cache.get("recon/fov", default=240.0)
    count = cache.get("recon/count", type=int)    # C05 numeric conversion
    cache.put("recon/progress", 0.5)
    cache.put_batch([("recon/a", 1), ("recon/a", 2)])
    exists = cache.contains("recon/fov")
    rows = list(cache.items("recon"))             # Eager owned local snapshot
    cache.detach()                                 # Re-attach remains possible
```

`put_batch` accepts either a Mapping in iteration order or an iterable of two-item pairs. Pair
iterables preserve order and duplicate keys; every entry is materialized and validated before one
canonical `:batch` submission. Successful non-empty writes submit first and then immediately apply
to the initiating cache. For duplicate keys, the last value serialized through each cache's local
sequencing path wins independently in the initiating cache and asynchronously updated peers. There
is no global publisher order or self-echo deduplication. Peer delivery is asynchronous, and success
is not an Issue #99 delivery barrier. Empty batches succeed without submission.

Reads and `items` are cache-local and perform no wire request. `items` uses raw-prefix matching,
lexical ordering, and returns an iterator over an eager owned snapshot that remains usable after
detach or close. Missing values, typed conversion, and Status exceptions use the same contract and
exception classes as ParamStore.

`detach` is idempotent and native-quiescent while leaving the object reusable. `close` is
idempotent, terminal, stops new operation admission, waits for already-admitted binding operations,
and returns
only after owned native resources are released. Operations after close raise
`ValueError("ParamCache is closed")`; a harmless repeated `detach` is permitted.

Stale/reconnect state, Python callbacks, and `WaitForLocalDelivery` remain
deferred to Issues #20, #26, and #99. Issue #27 provides
`ParamCache.get_array(key, *, dtype=...)` as a one-dimensional,
read-only zero-copy view over immutable cached BYTES. It accepts fixed-width numeric and boolean
NumPy dtypes, preserves explicit byte order without conversion, and does not infer or serialize
shape. The array keeps the exact cached value alive across overwrite, detach, close, and cache
destruction.

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
* ParamCache `get`, `contains`, and `items` are local C++ reads and keep the GIL. `items`
  materializes into C++-owned storage before constructing Python tuples
* Client Open, Attach, Detach, terminal Close/destruction, Put, and PutBatch release the GIL while
  native work may block. Python input conversion and result construction occur with the GIL held
* `ParamCache.get_array` retains the GIL while validating dtype and constructing the NumPy view;
  its ndarray owner releases only a C++ shared owner and never calls Python from a native callback
* In a StorageNode that uses a Python engine (§2.3), zenoh threads call into Python,
  so GIL acquisition occurs. State explicitly that C++ engines are recommended for production use

## 4. Type Stubs and Documentation

* Issue #27 installs `py.typed` and public `sitos/*.pyi` stubs and verifies them with blocking mypy;
  pyright remains nonblocking/deferred
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
