# sitos — Wire Protocol Specification

This document defines cross-language interoperability for sitos [C03].
A standard zenoh client can interoperate with sitos simply by following this specification.

## 1. Key space

### 1.1 Structure

```
<prefix>/base/<key>
<prefix>/session/<sid>/<key>
<prefix>/snap/<sid>/<key>
<prefix>/buffers/<sid>/durable/<key>
<prefix>/buffers/<sid>/ephemeral/<key>
<prefix>/meta/session/<sid>
<prefix>/meta/ack/<uuid>
<prefix>/base/:batch                 # batch delivery [ADR-0018]
<prefix>/session/<sid>/:batch        # batch delivery [ADR-0018]
```

* `<prefix>`: Default is `sitos`. Configurable. One or more zenoh chunks
* `<sid>`: session ID. `[0-9a-zA-Z_-]+` (UUID recommended). One chunk
* `<key>`: User key. One or more chunks (hierarchical keys allowed)
* `buffers/<sid>/{durable|ephemeral}/<key>`: route-selected session buffer values. `<sid>`
  and `<key>` reuse the existing grammar. The route requires the corresponding explicit Session
  capability. Payloads are plain `zenoh/bytes`, with no sitos schema or type tag.
* Buffer value routes support no inline `:batch`, `:fence`, snapshot, or other control segment.
  Accepted ADR-0029 defines Fence markers in a disjoint `meta/fence/**` namespace; #158 owns their
  planned implementation. They are never buffer values. Buffer values remain disjoint from
  ParamStore, ParamCache, ParamSubscription, and SessionView [ADR-0032].

> **Normative design; implementation planned:** Accepted ADR-0029 reserves these marker routes;
> #158 owns their production implementation.
>
> ```text
> <prefix>/meta/fence/cache/<sid>/<receiver-uuid>/<publisher-uuid>/<through>
> <prefix>/meta/fence/buffer/<sid>/<session-uuid>/<durable|ephemeral>/<publisher-uuid>/<applied|synced>/<through>
> ```

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

Authoritative Encoding rules are route-specific [C02]:

```
zenoh/bytes;sitos.v1          (single parameter value)
zenoh/bytes;sitos.v1.batch    (base/session batch, §5)
zenoh/bytes;sitos.v1.ack      (acknowledgement result, §6)
zenoh/bytes                   (durable or ephemeral buffer value)
```

> **Normative design; implementation planned:** Accepted ADR-0029 reserves
> `zenoh/bytes;sitos.v1.fence` as the authoritative same-publisher Fence marker Encoding in §6.1;
> #158 owns its production implementation.

Base, session, and snapshot parameter payload-v1 values use
`zenoh/bytes;sitos.v1`. Base and session batch values use
`zenoh/bytes;sitos.v1.batch`. Buffer routes always use bare `zenoh/bytes`: they
have no `sitos.v1` schema or type tag, and receivers keep their payloads
byte-opaque. Buffer handling has no schema-fallback warning path.

For parameter traffic, `Encoding` is type `zenoh/bytes` plus a schema suffix
[ADR-0016]. Senders emit the canonical slash spelling above. Parameter
receivers also accept the legacy `zenoh.bytes;<schema>` spelling and
schema-only identifiers for compatibility, but normalize recognized sitos
schemas to `sitos.v1`, `sitos.v1.batch`, or `sitos.v1.ack` in the transport-independent API.

Parameter receiver interpretation rules:

* schema is `sitos.v1` → decode according to this specification
* schema is absent/unknown → accept it as BYTES (raw value without a type tag) and
  log a warning as the parameter interoperability fallback

## 3. Mapping operations to keys

| Operation | zenoh operation | Key |
|---|---|---|
| Write value | `put` | Base, session, or a buffer route |
| Delete value | `delete` | `<prefix>/base/<key>` or `<prefix>/session/<sid>/<key>` |
| Read value | `get` | Base, session, snap, or durable buffer route |
| Prefix enumeration | `get` | `<prefix>/base/<chunk...>/**`, or a durable buffer route |
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

### 4.3 Buffer delivery and consistency

* Durable PUTs are write-once. A byte-identical repeat is idempotent; a conflicting repeat is
  protocol-invalid and StorageNode does not persist it. Ephemeral publishers follow the same
  contract, but no ephemeral value state is retained to enforce it.
* Zenoh live fanout and StorageNode persistence are not atomic. StorageNode is one subscriber and
  cannot retract a sample already observed by another subscriber. A conflicting raw PUT may
  therefore be observed live while durable Get retains the first bytes; consumers must reject that
  traffic. Invalid raw publications have no guaranteed live-observation semantics, and exactly-once
  delivery is not promised.
* Durable late join is normative: subscribe in buffering mode, perform synchronous Get and
  materialize replies, drain buffered samples under one ordering boundary, then enter live mode.
  Same-byte duplicates may be deduplicated during the transition. Ephemeral is live-only with no
  initial Get or replay guarantee.
* Buffer DELETE is unsupported in v0.4; Session lifecycle cleanup removes buffer state.
  CloseSession destroys engine ownership before returning, but physical directory removal is
  host-owned. A new v0.4 Session gets a fresh or logically empty store. Issue #108 owns restart
  catalogs and deletion retry.

### 4.4 read-only and admission rules

* Raw DELETE is supported for both base and session routes. The public ParamStore Delete API is
  base-only; snapshots remain read-only and buffer DELETE is unsupported in v0.4.
* put/delete to `snap/**`: StorageNode ignores it and logs a warning (no error response — zenoh
  put is fire-and-forget)
* Buffer capability checks and write-once validation are admission rules, not network ACLs.
* get for a nonexistent `<sid>`: 0 replies

## 5. Batch format (`sitos.v1.batch`)

Buffer routes never use this format; `:batch` is reserved for base and session parameter scopes.

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

> **Normative design; implementation in progress:** Accepted ADR-0028 owns this contract. The
> Transport boundary types, `AckAttachmentV1`, `AckResultV1`, the `sitos.v1.ack` Encoding, UUIDv4
> tokens, and `Status::OutcomeUnknown` below are implemented under Issue #14
> (DEC-14-ACK-ATTACHMENT-001). The StorageNode token lifecycle, `meta/ack/<uuid>` route behavior,
> and the one-submit/total-deadline helper are specified normatively by ADR-0028 and remain #14
> implementation work; Issue #17 owns the ParamStore `WriteOptions` policy layered on that contract.
> No implementation may introduce a second acknowledgement format.

Acknowledged Put and PutBatch use one data submission followed by bounded result polling:

1. sitos generates one canonical random UUIDv4 token per acknowledged operation and passes it to
   the Transport adapter through `PutOptions::ack_token`. The adapter encodes it as
   `AckAttachmentV1` and never invents a token; the high-level APIs never accept caller-selected
   tokens.
2. The adapter decodes a received attachment into the typed `TransportSample::ack` observation:
   absent (an acknowledgement-free write), a valid token, or malformed. A malformed attachment
   (unknown version, wrong length, or non-v4 UUID) is rejected before application and recorded as a
   protocol error; it never creates a result.
3. StorageNode claims the token before mutation, applies the operation, and retains an immutable
   `AckResultV1` in a bounded 4096-entry completion ring so it can answer
   `<prefix>/meta/ack/<uuid>` with Encoding `sitos.v1.ack`. Absent, Processing, evicted, and
   restart-lost tokens return zero replies.
4. The client polls only the acknowledgement query within one total deadline (query windows of
   `min(1000 ms, remaining)`, at least 100 ms apart, no attempt count) and never resubmits the data
   write. `Timeout` means no valid result was observed; none, some, or all effects may have
   occurred. `OutcomeUnknown` means StorageNode observed and attempted the operation but can make
   no stronger application claim.

Delete remains acknowledgement-free in v1; the adapter rejects `PutOptions::ack_token` on Delete
with `Status::InvalidArgument`, and a non-v4 token on Put is also `Status::InvalidArgument`.

#### AckAttachmentV1 (exactly 17 bytes)

```text
offset  size  field
0       1     schema_version = 1
1       16    UUID bytes in RFC 4122 network order (version 4, variant 10)
```

The canonical query spelling of the same token is lowercase `8-4-4-4-12` text; the wire carries
bytes, never text. Golden fixture: `tests/fixtures/ack_v1/attachment_put_token.hex`
(token `550e8400-e29b-41d4-a716-446655440000`).

#### AckResultV1 (`zenoh/bytes;sitos.v1.ack`)

```text
offset  size  field
0       1     schema_version = 1
1       1     operation_kind: put = 1, batch = 2, fence = 3
2       1     status: stable sitos Status numeric value
3       1     durability: applied = 1, synced = 2
4       4     applied_count_le
8       4     failed_index_le; UINT32_MAX means none/not applicable
12      8     through_sequence_le; 0 means not applicable
20      8     failed_sequence_le; UINT64_MAX means none/not applicable
28      4     message_length_le
32      n     sanitized UTF-8 message; 0 <= n <= 1024
```

The encoded length must equal `32 + message_length`. The closed Status allowlist is `Ok = 0`,
`NotFound = 1`, `TypeMismatch = 2`, `Disconnected = 4`, `ReadOnly = 5`, `InvalidKey = 6`,
`InvalidArgument = 7`, `Error = 8`, and `OutcomeUnknown = 9`; `Timeout = 3` is client-only and
rejected on the wire. Unknown versions, operation kinds, durability values, or Status values;
invalid sentinels; invalid UTF-8; and truncated, trailing, or overlong data are protocol errors.
Per-operation invariants follow the ADR-0028 table: Put and Batch always use `applied`; Put success
has `applied_count = 1` and no `failed_index`, Put failure has `applied_count = 0` and
`failed_index = 0`; Batch success has no `failed_index`, an envelope failure has
`applied_count = 0` and no `failed_index`, and an entry failure names the entry with
`applied_count` either `0` or equal to `failed_index` (the confirmed prefix); Fence has
`applied_count = 0`, no `failed_index`, and a `failed_sequence` that is either none or nonzero and
no greater than `through_sequence`. Put and Batch leave both sequence fields not applicable.
Golden fixtures: `tests/fixtures/ack_v1/result_*.hex`.

### 6.1 Same-publisher Fence control

> **Normative design; implementation planned:** Accepted ADR-0029 owns this contract; production
> implementation and executable qualification belong to #158.

Covered data retains its existing key, payload, and route Encoding and carries the exact ordering
attachment below. An absent attachment is an ordinary write outside a sitos Fence prefix.

```text
FenceLaneAttachmentV1 — exactly 25 bytes

offset  size  field
0       1     schema_version = 1
1       16    logical Publisher UUIDv4 in RFC 4122 network order
17      8     nonzero sequence_le
```

Fence markers use the `meta/fence/**` routes in §1, Encoding
`zenoh/bytes;sitos.v1.fence`, and the exact one-byte payload `01`. UUID route chunks are lowercase
canonical UUIDv4 text. `<through>` is canonical unsigned decimal in the `uint64_t` range; zero
represents an empty covered prefix. `synced` is invalid for ephemeral buffers. A buffer
`session-uuid` is a generated random UUIDv4 that identifies one successful `CreateSession`
incarnation under its collision-resistant non-collision condition; same-SID recreation generates a
fresh value. StorageNode rejects a mismatched generation before ACK-token claim or marker completion;
no AckResult is created, so under the ACK-token non-collision condition the caller can only time out. Covered-data
`FenceLaneAttachmentV1` remains exactly 25 bytes, and delayed old-generation data isolation is not a
v1 Fence guarantee.

Every marker carries ADR-0028's exact 17-byte `AckAttachmentV1`; that token is not repeated in the
key or payload. Cache-target markers directly complete only the named ParamCache Attach generation
and create no StorageNode result. Buffer-target markers create ADR-0028 `AckResultV1` through the
existing `meta/ack/<uuid>` query contract. The marker route identifies the receiver target and generation, Publisher, requested durability,
and covered sequence without changing parameter or buffer values.

A valid Fence requires one serialized logical Publisher lane and the ADR-0029 reliable,
`Block`/`Data`/non-express profile for data and marker. The ordering guarantee is conditional on
generated Publisher UUIDs being distinct: a receiver cannot distinguish same-UUID traffic that
presents the next valid sequence. Under that condition, success requires contiguous receiver
processing through `<through>`, not marker arrival; a missing, reordered, duplicate, malformed, or
unprovable covered sequence fails closed and is never reported as success. Multiple logical
Publishers may share a session but have independent UUID/sequence lanes only under the same
collision-resistant non-collision condition; no cross-Publisher order is claimed. Generated UUIDv4
ACK tokens are likewise collision-resistant rather than collision-impossible; distinct generated
tokens are a waiter/result-isolation condition, and same-token collisions remain the residual risk
defined by ADR-0029. A Transport-generation replacement permanently disconnects the existing
Publisher and requires a new UUID/sequence lane. For buffer markers, successful atomic active-Session
admission linearizes before CloseSession; a Closing-first attempt returns `InvalidArgument`. See
ADR-0029 for exact validation, lifecycle, bounded-state, result, and topology rules.

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
