# sitos — Wire Protocol Specification

This document defines cross-language interoperability for sitos [C03].
A standard zenoh client can interoperate with sitos simply by following this specification.

## 1. Key space

### 1.1 Structure

```
<prefix>/base/<key>
<prefix>/session/<sid>/<key>
<prefix>/snap/<sid>/<key>
<prefix>/buffers/<sid>/<key>
<prefix>/meta/session/<sid>
<prefix>/meta/ack/<uuid>
<prefix>/base/:batch                 # batch delivery [ADR-0018]
<prefix>/session/<sid>/:batch        # batch delivery [ADR-0018]
```

* `<prefix>`: Default is `sitos`. Configurable. One or more zenoh chunks
* `<sid>`: session ID. `[0-9a-zA-Z_-]+` (UUID recommended). One chunk
* `<key>`: User key. One or more chunks (hierarchical keys allowed)
* `buffers/<sid>/<key>`: session-scoped large binary values. `<sid>` and `<key>` follow the
  same grammar as other scopes. No `:batch` and no `snap` counterpart. The persistence mode
  (durable/ephemeral) is a host-side session option and is **not** encoded on the wire [ADR-0014]

### 1.2 User-key grammar

```
key    = chunk *( "/" chunk )
chunk  = 1*( ALPHA / DIGIT / "_" / "-" / "." )
```

* Prohibited: empty chunks, leading or trailing `/`, whitespace, and every character outside
  the grammar above. In particular, `:batch` is a reserved control segment and cannot be a
  user key
* Case-sensitive
* Recommended: represent hierarchy with `/` (example: `recon/fov`).
  Legacy `.`-separated keys (example: `recon.fov`) are legal as one chunk, but
  cannot be partially enumerated with zenoh wildcards (§4.2)

### 1.3 Migration rule for legacy keys (informative)

When moving keys from the legacy parameter store to sitos, a mechanical `.` → `/`
conversion is recommended. The conversion is the responsibility of an external adapter layer;
sitos itself is not involved.

## 2. payload v1 encoding

### 2.1 Single value

```
offset  size  Contents
0       1     Type tag (uint8)
1       n     Value body (little-endian)
```

| Type tag | Name | Value body | Corresponding type (C++ / Python) |
|---|---|---|---|
| 0 | BOOL | 1 byte (0=false, nonzero=true) | `bool` / `bool` |
| 1 | S64 | 8-byte signed integer LE | `std::int64_t` / `int` |
| 2 | DP | 8-byte IEEE754 double-precision LE | `double` / `float` |
| 3 | STR | UTF-8 byte sequence (length derived from payload length; no NUL terminator) | `std::string` / `str` |
| 4 | BYTES | Raw byte sequence | `std::vector<std::byte>` / `bytes`, `numpy.ndarray` |

* Type tag values match the type enumeration order of the preceding legacy parameter store
  (BOOL, S64, DP, STR, BYTES) [C01]
* Numeric-array interpretation of BYTES (for example, a float32 LUT) is the reader's
  responsibility (same as the legacy parameter store). Element-type metadata is not included
  in v1
* 5–127 are reserved for future types. 128–255 are unused

### 2.3 Golden fixtures

All implementations (C++ codec / Python binding / zenoh-python interop) must match the
following fixtures **byte-for-byte**. The hexadecimal notation represents the entire payload
(including the type tag).

| fixture name | Value | Type | payload hex |
|---|---:|---|---|
| `bool_false` | `false` | BOOL | `00 00` |
| `bool_true` | `true` | BOOL | `00 01` |
| `s64_zero` | `0` | S64 | `01 00 00 00 00 00 00 00 00` |
| `s64_minus1` | `-1` | S64 | `01 ff ff ff ff ff ff ff ff` |
| `s64_i32max` | `2147483647` | S64 | `01 ff ff ff 7f 00 00 00 00` |
| `dp_zero` | `0.0` | DP | `02 00 00 00 00 00 00 00 00` |
| `dp_240` | `240.0` | DP | `02 00 00 00 00 00 00 6e 40` |
| `dp_nan` | quiet NaN | DP | `02 00 00 00 00 00 00 f8 7f` |
| `str_empty` | `""` | STR | `03` |
| `str_ascii` | `"abc"` | STR | `03 61 62 63` |
| `str_utf8` | `"穀"` | STR | `03 e7 a9 80` |
| `bytes_empty` | `[]` | BYTES | `04` |
| `bytes_0102ff` | `[0x01,0x02,0xff]` | BYTES | `04 01 02 ff` |

For `dp_nan`, use the bit pattern in the table above as the canonical fixture for an IEEE754
quiet NaN. Compare decoded values with `isnan()`, and normalize to the canonical NaN above
when re-encoding.

### 2.2 zenoh Encoding

Set the following zenoh `Encoding` on put/reply [C02]:

```
zenoh/bytes;sitos.v1          (single value)
zenoh/bytes;sitos.v1.batch    (batch, §5)
```

(`Encoding` = type `zenoh/bytes` + schema suffix) [ADR-0016]

Senders emit the canonical slash spelling above. Receivers also accept the
legacy `zenoh.bytes;<schema>` spelling and schema-only identifiers for
compatibility, but normalize recognized sitos schemas to `sitos.v1` or
`sitos.v1.batch` in the transport-independent API.

Receiver interpretation rules:

* schema is `sitos.v1` → decode according to this specification
* schema is absent/unknown → accept it as BYTES (raw value without a type tag) and log a warning.
  This lets a plain zenoh client still exchange the value as a binary value even if it forgets to
  set Encoding (interoperability fallback)

## 3. Mapping operations to keys

| Operation | zenoh operation | Key |
|---|---|---|
| Write value | `put` | `<prefix>/base/<key>` or `<prefix>/session/<sid>/<key>` |
| Delete value | `delete` | Same as above (valid only for base; not allowed for snap) |
| Read value | `get` | Same key as for writes, or `<prefix>/snap/<sid>/<key>` |
| Prefix enumeration | `get` | `<prefix>/base/<chunk...>/**`, etc. |
| Batch write | `put` (batch payload) | `<prefix>/base/:batch` / `<prefix>/session/<sid>/:batch` (§5) |

## 4. Query semantics

### 4.1 Single-key get

Get with an exact-match key. If no key matches, there are 0 replies (not an error).

### 4.2 Enumeration (List)

zenoh wildcards operate on chunks (`*` = one chunk, `**` = zero or more chunks).
**Partial prefix matching inside a chunk cannot be represented on the wire**.

* Enumeration at a chunk boundary: `get("sitos/base/recon/**")` — StorageNode replies using
  the engine's `List("recon/")`
* Non-boundary prefixes (such as the legacy-compatible API's `List("recon.f")`):
  the client library gets the parent scope (`sitos/base/**`) and filters on the client side.
  This is not specified as part of the wire protocol

### 4.3 read-only rules for snap / session

* put/delete to `snap/**`: StorageNode ignores it and logs a warning (no error response — zenoh
  put is fire-and-forget)
* get for a nonexistent `<sid>`: 0 replies

## 5. Batch format (`sitos.v1.batch`)

Delivers multiple entries in one zenoh put [F09].
Put to key `<prefix>/base/:batch` or `<prefix>/session/<sid>/:batch`, and store all entries in
the payload. `:batch` is reserved because it is zenoh-valid while `$batch` is not; see
[ADR-0018](adr/0018-use-zenoh-valid-batch-key-segment.md).

```
offset  size  Contents
0       4     Entry count N (uint32 LE)
Repeated N times thereafter:
        4     Key length kLen (uint32 LE)
        kLen  Key (UTF-8; relative key with <prefix>/... removed)
        1     Type tag
        4     Value length vLen (uint32 LE)
        vLen  Value body
```

* StorageNode validates and materializes all entries before its first engine write. It then applies
  them in encoded order before processing the next subscriber message. Invalid batches perform no
  writes; an engine write failure is logged and does not roll back an earlier successful write.
  This sequencing does not provide reader atomicity: a concurrent get or list may observe a
  partially applied batch.
* Subscribers (ParamCache) also subscribe to the batch key and apply the same format
  (because it is included in the normal subscription ranges `session/<sid>/**` and `base/**`,
  it can be received through the same path as ordinary delta subscriptions)
* Subscribers that need per-key notifications expand the batch before handling it

> Design note: The field order is aligned with the delivery wire format of the preceding legacy
> parameter store (num + [keyLen, key, type, len, data]...), which simplifies migration-adapter
> implementation.

### 5.1 Batch fixture

`batch_base_two_entries`:

* put key: `sitos/base/:batch`
* entries:
  1. key=`recon/fov`, value=`DP 240.0`
  2. key=`recon/kernel`, value=`STR "sharp"`
* payload hex:

```
02 00 00 00
09 00 00 00 72 65 63 6f 6e 2f 66 6f 76 02 08 00 00 00 00 00 00 00 00 00 6e 40
0c 00 00 00 72 65 63 6f 6e 2f 6b 65 72 6e 65 6c 03 05 00 00 00 73 68 61 72 70
```

## 6. ack protocol (Put completion confirmation)

A Put with `PutOptions::ack = true` follows this procedure:

1. The client generates an ack token (UUID) corresponding to the beginning of the put payload and
   attaches `ack=<uuid>` to the zenoh put `attachment`
2. After completing the apply, StorageNode keeps a completion record in a ring buffer (most recent
   4096 entries) so it can respond to get on `<prefix>/meta/ack/<uuid>`
3. After the put, the client performs `get("<prefix>/meta/ack/<uuid>")`; if there is a reply,
   completion is confirmed. On timeout (default 1 s), it retries up to 3 times

Clients that do not support attachment are treated as ack-less put clients (compatible behavior).

## 7. meta keys

### 7.1 `meta/session/<sid>`

For checking session existence and debugging. StorageNode creates it on CreateSession.
The value is JSON encoded as payload v1 STR:

```json
{"state": "active", "created_at": "2026-07-07T01:23:45Z"}
```

Deleted by CloseSession.

## 8. Versioning [C04]

* This specification is **wire v1**. Schema suffixes `sitos.v1*` are the identifiers
* Backward-compatible additions (new meta keys, new type tags) are minor versions
* Changes to or deletion of meanings of existing fields are major versions (change the schema to
  `sitos.v2*`, and provide an implementation supporting both during the migration period)

(END OF DOCUMENT)
