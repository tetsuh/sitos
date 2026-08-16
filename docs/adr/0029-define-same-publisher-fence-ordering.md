# ADR-0029: Define same-publisher Fence ordering

## Status

Accepted — 2026-08-16

## Context

ParamCache local-delivery waits (#99) and BufferPublisher applied or synchronized fences (#107)
need one ordering primitive that covers only operations submitted by the initiating logical
Publisher before a precise boundary. A Zenoh session is not that logical identity, different data
and control keys may have different delivery paths, and callback arrival does not prove receiver
processing completion. ADR-0032 also requires buffer keys and opaque `zenoh/bytes` values to remain
unchanged. The contract therefore needs explicit Publisher identity and sequence evidence while
reusing ADR-0028's acknowledgement token, result, retention, query, and deadline protocol. Issue
#158 owns production implementation and executable qualification under this Accepted ADR.

## Decision

We will serialize each sitos logical Publisher as its own UUIDv4-and-sequence lane, attach that lane
identity to every covered data publication, and send one target-aware in-band Fence marker after the
covered prefix. A Fence succeeds only after its designated receiver has completed every sequence in
that prefix; marker arrival without contiguous completion is never sufficient. The marker will use
ADR-0028's exact `AckAttachmentV1` and `AckResultV1`, while covered data will use the distinct
ordering-only `FenceLaneAttachmentV1` approved by DEC-106-LANE-ATTACHMENT-001.

### Logical Publisher identity

One logical Publisher is one move-only sitos-owned lane state containing:

* one internally generated canonical random UUIDv4;
* one immutable receiver binding: cache `(sid, Attach-generation UUID)` or buffer
  `(sid, durable|ephemeral)`;
* one `uint64_t last_sequence`, initially zero, plus one exhausted flag;
* one admission gate and one lane mutex;
* at most one in-flight Fence token/waiter;
* one retained local possible-submission diagnostic; and
* the Transport generation used for every covered data publication and marker.

The UUID is not caller-selectable, a credential, an ACL, or a Zenoh entity id. Random UUIDv4
generation makes it probabilistically distinct (collision-resistant), not collision-impossible, across
live and restarted processes. A supported caller cannot intentionally reuse or spoof it. Receiver state
has no discriminator beyond the bound Publisher UUID, so an accidental generated UUID collision is not
directly detectable. The ordering guarantee is therefore conditional on generated Publisher UUIDs
being distinct. Stale, duplicate, unexpected, or malformed sequence observations, including those
caused by raw traffic or a collision, poison that receiver lane and fail closed; same-UUID traffic presenting
the next valid sequence is indistinguishable and is not authenticated. Raw Zenoh writers can still
cause denial or injection on writable routes, so Fence adds ordering evidence rather than
authentication.

Move construction transfers the same state and identity. Non-concurrent move assignment first
closes the destination admission, atomically completes its waiter with `Disconnected`, quiesces its
admitted calls, and then transfers the source state. A moved-from Publisher is disconnected. Moving
an object concurrently with one of its calls is an unsupported caller data race. Destruction closes
admission, atomically completes its waiter with `Disconnected`, waits admitted calls to quiesce,
and then releases the identity. Session close or Transport-generation replacement uses the same
cancel-before-quiesce order. No lane state, sequence, waiter, or failure state is persisted or
intentionally reused after process restart; each fresh identity remains subject to the UUID non-collision
condition above.

The Publisher UUID is distinct from every per-Fence ADR-0028 correlation token. Its receiver
binding is fixed at creation and cannot be changed or reused for another target, SID, class, or
Attach generation. A public object spanning multiple receiver bindings owns a separate internal
Publisher UUID/sequence lane for each binding. Move transfers the binding unchanged.

Multiple logical Publishers with distinct generated UUIDs may share one Fence-capable Transport or
Zenoh session because their UUID and sequence states remain separate under the non-collision condition.
Their calls may interleave, and no cross-Publisher order is promised. One logical Publisher may not
split its data and marker across Transport generations
or Zenoh sessions.

### Covered-data attachment

A covered data publication carries this exact Zenoh attachment:

```text
FenceLaneAttachmentV1 — exactly 25 bytes

offset  size  field
0       1     schema_version = 1
1       16    Publisher UUID bytes in RFC 4122 network order
17      8     sequence_le; valid range 1..UINT64_MAX
```

The length must be exactly 25, the version must be 1, the UUID must have the RFC 4122 variant and
version 4 bits, and sequence zero is invalid. Wrong length, unknown version, invalid UUID, or zero
sequence is rejected before covered receiver application. Malformed classification independently
reports an unambiguously recoverable canonical Publisher UUID and, only for a version-1 candidate
with all eight sequence bytes present, an optional valid nonzero sequence. A retainable identified
lane latches `Status::Error` at that sequence when present; when only the UUID is recoverable, it
latches a lane-global `Error` represented in ADR-0028 by `failed_sequence = UINT64_MAX`. The global
failure applies to every through value. If the bounded receiver cannot retain a newly identified
lane, the capacity-poison rule below applies instead. If no Publisher can be identified, no lane
advances. An absent attachment means an ordinary write that is not part of a sitos Fence prefix.

Route context disambiguates this 25-byte ordering attachment from ADR-0028's exact 17-byte
`AckAttachmentV1`. Parameter Put/PutBatch covered for the initiating ParamCache use their existing
key, payload, and Encoding. On base/session parameter routes, only the matching initiating
ParamCache interprets a valid 25-byte attachment for Fence accounting; StorageNode, peer caches,
and ordinary subscribers ignore it as ordering metadata and continue their normal parameter
application. A malformed would-be lane attachment can fail only an identifiable initiating local
Fence path and never invents a StorageNode ACK result. Buffer Push uses its existing ADR-0032 route,
opaque payload, and bare `zenoh/bytes` Encoding; StorageNode is the lane-aware receiver on those
routes. Marker routes accept only the 17-byte `AckAttachmentV1`.

One PutBatch wire message consumes one sequence and reaches its processing point only after the
complete decoded batch path finishes. A future operation that needs both a per-data acknowledgement
and Fence coverage requires a later ADR because one Zenoh sample has one attachment; v1 does not
compose the two attachment formats.

Raw subscribers receive unchanged keys, Encodings, and payload bytes and may ignore the attachment.
Raw attachment-free writers remain interoperable ordinary writers, but their operations cannot be
claimed by a sitos Fence.

Issue #158 adds one transport-independent `FenceLaneMetadata` value containing the 16 UUID bytes and
`uint64_t sequence`. `PutOptions` carries an optional valid `fence_lane`; `TransportSample` carries
a tagged `FenceLaneObservation` of absent, valid metadata, or malformed candidate with independently
optional recoverable identity and valid nonzero v1 sequence. The Put field is mutually exclusive
with ADR-0028 per-data acknowledgement metadata. Only the adapter encodes/decodes the 25-byte form and classifies candidates; ParamCache
and StorageNode consume the typed observation and never raw zenoh-c types. This seam, its mocks, and
its codec tests are mandatory #158 targets.

### Fence marker routes and bytes

Fence uses one common Encoding and payload schema with two target-specific route shapes:

```text
<prefix>/meta/fence/cache/<sid>/<receiver-uuid>/<publisher-uuid>/<through>

<prefix>/meta/fence/buffer/<sid>/<durable|ephemeral>/<publisher-uuid>/
    <applied|synced>/<through>
```

The line break in the buffer form is for display only. The wire key has no whitespace. `<sid>` uses
the existing Session-id grammar. Both UUID chunks are lowercase canonical UUIDv4 text. `<through>`
is canonical unsigned decimal `0` or `[1-9][0-9]*` in the `uint64_t` range; leading zeros and signs
are invalid. `synced` is invalid for `ephemeral`. The cache route has implicit local-delivery
semantics and therefore no durability chunk. The receiver UUID identifies one ParamCache Attach
generation and is generated and owned under the same UUID rules as Publisher identity.

The marker uses:

```text
Encoding: zenoh/bytes;sitos.v1.fence

FenceMarkerV1 payload — exactly 1 byte

offset  size  field
0       1     schema_version = 1
```

Every marker carries ADR-0028's exact 17-byte `AckAttachmentV1`. That attachment contains the sole
per-Fence correlation token. Target, receiver identity, Publisher identity, requested durability,
and covered sequence are represented by the route; the token is not duplicated in the route or
payload. The payload version allows compatible marker-envelope evolution without changing buffer
or parameter values.

This is one target-aware marker contract rather than two protocols: both routes share Encoding,
payload, UUID, sequence, submission, duplicate, and timeout rules. The variants are necessary
because the cache target identifies one local Attach generation and completes a direct waiter,
whereas the buffer target identifies one StorageNode Session/class lane and creates an ADR-0028
result.

Invalid route grammar, wrong Encoding, wrong payload length, unknown payload version, invalid
attachment, unsupported class/durability, or noncanonical field is a protocol error. For a buffer
marker, a valid ACK token and recoverable route fields produce one `Status::Error` AckResult;
without a recoverable valid token no result can be created. For a cache marker, a recoverable token
completes only its matching waiter with `Status::Error`. A cache ignores buffer-target completion,
a StorageNode ignores cache-target completion, and a cache with a different receiver UUID ignores
the marker with a bounded diagnostic. A valid buffer marker for a missing Session produces
`Status::NotFound`.

A duplicate retained buffer token follows ADR-0028: the same fingerprint never repeats processing
and returns the immutable prior result; a different fingerprint is a collision. A late duplicate
after ACK-result eviction has only ADR-0028's limited guarantees. A cache removes a token when its
waiter completes or is cancelled, so duplicate or late markers with no matching waiter are ignored
and cannot complete a newer call.

Representative wire examples are normative:

| Example | Outcome |
|---|---|
| `sitos/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/7`, payload `01`, valid ACK attachment | valid cache marker through 7 |
| `sitos/meta/fence/buffer/s1/durable/8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/applied/7`, payload `01` | valid durable applied marker |
| same buffer route with `synced/7` | valid durable synchronized marker after #105 capability exists |
| valid cache or buffer route ending `/0` | valid empty prefix; no receiver lane allocation |
| route ending `/07`, `/-1`, or a value above `UINT64_MAX` | invalid noncanonical sequence |
| `buffer/s1/ephemeral/.../synced/7` | invalid target/durability combination |
| UUID chunk with uppercase, non-v4 bits, or noncanonical text | invalid route |
| 24- or 26-byte lane attachment, version 2, non-v4 UUID, or sequence zero | invalid covered-data attachment |
| marker payload `02` or a payload length other than one | unsupported/malformed marker version |
| malformed buffer marker with valid ACK token and recoverable route | one `Error` AckResult |
| malformed marker without recoverable valid ACK token | no result; caller can only time out |

### Sequence allocation and Fence linearization

Definite local validation and capability rejection occurs before lane admission, consumes no
sequence, invokes no Transport method, and is not covered by a later Fence. Every data operation for
which Transport is invoked consumes exactly one sequence, even if Transport returns non-OK, because
submission may have occurred. Data is never resubmitted.

For data, the Publisher:

1. validates all inputs that can be validated without Transport;
2. acquires admission and the lane mutex;
3. rejects when exhausted; otherwise increments `last_sequence` immediately before the sole
   Transport invocation and sets exhausted after allocating `UINT64_MAX`;
4. attaches `FenceLaneAttachmentV1` with that sequence and invokes Put once through the Transport
   submission sequencer; and
5. retains a non-OK post-invocation diagnostic as `MayHaveSubmitted`, then releases the mutex.

For Fence, the Publisher:

1. requires a positive total deadline and validates the target and Fence capability;
2. acquires admission and the same lane mutex after earlier admitted data calls;
3. snapshots `through_sequence = last_sequence`;
4. creates one ADR-0028 token and publishes the token/waiter state before Transport can invoke a
   synchronous callback;
5. linearizes the Fence when that waiter and `through_sequence` become immutable while the lane
   mutex is held, immediately before the sole marker Put;
6. starts the total deadline immediately before invoking the sole marker Put, after waiter
   publication;
7. submits the marker once through the same Transport generation and submission sequencer; and
8. releases the lane mutex so later data can allocate larger sequences excluded from this Fence.

A second Fence while one is pending is a definite local `Status::InvalidArgument` with
`std::errc::operation_in_progress`; it creates no token or marker. Later data may proceed while the
first Fence waits because its larger sequences are excluded. After allocating `UINT64_MAX`, the
exhausted flag rejects later data and no wrap is allowed; Fence continues to snapshot the retained
`last_sequence = UINT64_MAX` until the caller creates a new Publisher.

`through_sequence = 0` is a valid empty prefix and allocates no receiver lane. It succeeds only
when no positive sequence for that binding has crossed the marker. After marker, token, receiver,
active Session, class, and capability validation, a cache target, buffer `applied`, or
durable-buffer `synced` Fence then completes `Ok` without invoking a durability barrier. If a
positive sequence completed first, the empty marker is an order violation and returns
`OutcomeUnknown`. Ephemeral `synced` remains invalid. Empty success proves only that the designated
receiver processed the marker against a valid target; it covers no data.

A non-OK marker Put after invocation is `MayHaveSubmitted`. The cache target continues observing
its direct waiter and the buffer target continues ADR-0028 result queries within the same total
deadline. Any valid observed completion takes precedence over the local submission error; without
one, the call returns `Timeout` with the latest local/native cause. The marker is never resubmitted.

The waiter lock is never held while invoking Transport. A synchronous local marker callback may
therefore find the published waiter, and receiver processing never acquires the sender lane mutex.
A Fence callback, LogSink, engine, or application callback must not synchronously call a covered
Put or Fence on the same Transport, or wait for another task that does so. Posting nonblocking work
for later execution remains supported.

### Transport and Zenoh ordering contract

Sitos selects one-shot session puts because data keys vary and markers use a reserved control key.
A declared Zenoh Publisher has one fixed key expression and is neither the sitos logical identity
nor a complete cross-key solution.

A Fence-capable Transport enforces this profile for a Publisher's data and marker:

| Property | Required outcome |
|---|---|
| Transport generation | exactly the same for data and marker |
| Zenoh reliability | reliable channel |
| Congestion control | `Block` |
| Priority | `Data` for both data and marker |
| Express | false for both; normal batching may occur |
| Sender invocation | sitos Transport-wide FIFO submission sequencer |
| Receiver invocation | bounded FIFO dispatch lane shared by the target's data/control callbacks |
| Resubmission | prohibited for both data and marker |

A Transport must reject Fence capability before submission with `Status::InvalidArgument` if it
cannot enforce the profile, if key-specific configuration can override data and marker differently,
or if an externally injected/shared session cannot reserve the required submission and callback
sequencers. The stable implementation must not depend on Zenoh's unstable SourceInfo or native
Publisher-id APIs.

Attachment classification is disjoint before dispatch: exactly 17 bytes is reserved as an
ADR-0028 candidate and is never lane metadata; exactly 25 bytes is a Fence-lane candidate including
unknown versions; any other nonempty length whose first byte is version 1 is a malformed v1 lane
candidate. Marker routes accept only a valid 17-byte `AckAttachmentV1`. The 24- and 26-byte negative
examples are therefore unambiguous, while ADR-0028-only data bypasses Fence dispatch.

Fence participation also depends on receiver registration, not bytes alone:

* the initiating ParamCache dispatch group admits only data metadata whose recoverable Publisher
  UUID matches its immutable `(sid, Attach generation)` binding, plus markers naming that receiver;
* StorageNode admits lane candidates only on durable/ephemeral buffer data routes and buffer-target
  markers; attached base/session parameter data follows normal StorageNode application;
* a peer ParamCache or ordinary subscriber bypasses Fence accounting for another receiver binding
  and follows normal delivery; and
* an unidentifiable malformed parameter attachment cannot claim an initiating lane, follows normal
  parameter delivery, and can never contribute to a later Fence success.

Issue #158 therefore registers an explicit receiver-aware Fence dispatch binding with each internal
data/control callback group. Only callbacks selected by that binding enter the bounded lane.
Attachment-free data, ADR-0028-only data, and other raw metadata retain their existing
callback/application semantics and never consume its capacity. Dispatch overflow cannot drop or
reject unrelated interoperable writes.

The receiver dispatch lane atomically admits at most 256 participating callback entries, assigns
each admitted entry a ticket, and invokes component callbacks in ticket order. It uses fixed
counters and permits, not an unbounded sample queue. Classification, entry admission, and overflow
proof are serialized under the receiver registration lease. When a valid covered-data observation
arrives at the full bound, it is not applied, but before rejection returns a synchronous
rejection-proof transaction must update an existing receiver lane or create one within its normal
bound. It first raises `highest_observed_sequence`, then applies the normal sequence disposition
without application: a stale sequence latches `Error` at that sequence, an expected sequence latches
`OutcomeUnknown` at that sequence, and a future sequence latches `OutcomeUnknown` at the earlier
missing expected sequence.

A malformed candidate at the full dispatch bound uses the same synchronous proof transaction but
retains the malformed-data disposition instead of the valid-data sequence-position disposition: a
recoverable UUID and valid nonzero N raises `highest_observed_sequence` and latches `Error` at N;
a recoverable UUID alone latches lane-global `Error` with the sentinel. This transaction holds no
dispatch entry and is serialized one at a time with admission, so overflow cannot create another
unbounded queue or callback set.

If the identifiable data belongs to a new StorageNode Publisher UUID and the 4096-lane registry is
also full, the transaction cannot retain that UUID. It instead sets one O(1)
`unretained_rejection` poison bit on the matching active `(sid, durable|ephemeral)` scope. A first
marker for any absent lane in that scope must consult the bit; even an empty-prefix marker then
returns `OutcomeUnknown` rather than claiming that no positive sequence crossed. The bit retains
neither the rejected UUID nor its sequence and no later result may name that rejected sequence.
For any absent lane, a positive marker instead fails at the independently unproven sequence 1;
only an empty marker needs the poison and uses the sentinel. The bit clears only with that
Session/class state at CloseSession or Stop. ParamCache has one fixed lane per Attach
generation and therefore records an identifiable overflow directly in that lane. If lifecycle
cleanup prevents either proof transaction, the receiver cannot later complete a marker from that
registration. An unadmitted marker creates no completion and its caller can only reach `Timeout`.
Attachment-free and otherwise nonparticipating writes neither set this poison nor consume dispatch
capacity.

Undeclare or session close stops dispatch admission, rejects new entries, and quiesces admitted
callbacks before destroying dispatch state. Therefore a marker whose ticket follows a covered data
callback cannot complete before that callback returns from its designated processing path. Native
delivery that presents the marker before a missing or reordered data sequence is detected by the
Publisher sequence and cannot produce success.

Zenoh 1.9.0 source defines `Reliability::Reliable` as the default and describes reliable messages as
guaranteed delivery. Zenoh-c defines `Block` as not dropping under congestion, while `Drop` may
discard, and its reliability selector is unstable and only a link-selection marker in that release.
Zenoh also applies key-expression QoS overrides after builder options. These facts do not establish
the complete sitos cross-key or callback-completion guarantee. Sitos therefore owns serialization,
profile validation, callback completion, and UUID/sequence gap proof. Tagged 1.9.0 source is the
baseline evidence; later supported 1.x versions are accepted only after the minimum/latest
qualification required by the dependency policy. No experiment or later 1.x result broadens this
normative sitos contract.

| Topology or QoS | Fence outcome |
|---|---|
| One logical Publisher on one session | supported under the required profile |
| Multiple logical Publishers sharing one session | supported when generated UUIDs differ; UUID/sequence lanes isolate them |
| Different logical Publishers on distinct sessions | supported independently under the same UUID non-collision condition; no cross-session order |
| One logical Publisher split across sessions/generations | rejected before marker submission |
| Publisher and receiver sharing one session | supported; waiter prepublication handles loopback |
| Distinct publisher and receiver sessions | supported when the receiver has a reliable compatible path |
| Best-effort reliability or `Drop` congestion | Fence capability rejected |
| Different priority or express behavior for data/marker | Fence capability rejected |
| Unknown or uninspectable QoS override | Fence capability rejected |
| Missing, reordered, or unprovable sequence | never success; first unprovable sequence fails |

### Receiver sequence state and processing boundaries

A receiver lane is keyed by its target plus Publisher UUID. Buffer lanes additionally include SID
and durable/ephemeral class; cache lanes additionally include the receiver Attach generation.
Receiver rules are monotonic and fail closed:

| Observation | Receiver action |
|---|---|
| absent lane receives sequence 1 | admit the lane and process once |
| sequence equals next expected | process once; then advance even when a terminal failure is latched |
| sequence is below next expected | do not reapply; latch `Error` at that sequence |
| sequence is above next expected | do not apply; latch `OutcomeUnknown` at the missing expected sequence |
| malformed lane attachment | do not apply; for a retainable identified lane latch `Error` at recoverable valid N, or lane-global with the sentinel when only its UUID is recoverable |
| lane already failed | do not restore success; later absolute prefixes retain the first failure |
| first marker has through below completed prefix after no duplicate/global/in-prefix failure was selected | later data overtook the marker; fail `OutcomeUnknown` with failed-sequence sentinel |
| first marker has through above completed prefix after no duplicate/global/in-prefix failure or higher observation was selected | fail at the first missing or unprovable sequence |
| retained duplicate token after later data | return the original immutable ADR-0028 result before applying the order test |

First-marker evaluation is ordered and normative. Retained ADR-0028 duplicate-token lookup occurs
first and returns the original immutable result. Otherwise a retained lane applies these steps:

1. a lane-global first failure returns its retained Status with
   `failed_sequence = UINT64_MAX` for every through value;
2. a sequence-specific first failure N with `N <= through_sequence` returns its retained Status and N;
3. otherwise, `completed_through > through_sequence` or
   `highest_observed_sequence > through_sequence` proves that excluded later data crossed before
   the marker and returns `OutcomeUnknown` with `failed_sequence = UINT64_MAX`;
4. otherwise, `completed_through < through_sequence` returns `OutcomeUnknown` at the first missing
   sequence; and
5. exact completed-prefix equality with no remaining failure returns success.

A failure above the marker's through value remains retained for later Fences but cannot be selected
as this Fence's sequence failure; its higher observation reaches step 3. A selectable first failure
therefore wins over simultaneous marker-overtake evidence, while an out-of-prefix sequence is never
named in ADR-0028. An absent-lane empty-prefix success additionally requires that the active
StorageNode Session/class scope have no `unretained_rejection` poison; a set bit returns
`OutcomeUnknown` with the failed-sequence sentinel.

A first failure is retained for the identity's lifetime. Because later Fence values name an absolute
prefix, a successful or failed Fence does not clear it. Recovery after a covered failure requires a
new Publisher identity; sequences never reset within an identity. This is conservative and does not
claim network exactly-once delivery.

For the initiating ParamCache target, the processing point is completion of the cache subscriber's
normal serialized decode and mutation path under its callback lease. A batch reaches the point only
after all entries finish. It is not Zenoh arrival, Transport callback entry, the initiating write's
direct local apply, peer-cache delivery, StorageNode application, or an application subscriber
callback. Only the ParamCache Attach generation named by `receiver-uuid` tracks that Publisher lane.
Peer caches treat the attachment as ordinary metadata and cannot complete the initiating waiter.
The marker callback runs on the same Transport dispatch lane, evaluates contiguous completion, and
directly completes the matching local waiter. It creates no StorageNode result and performs no ACK
query.

For the StorageNode buffer target, the processing point is completion of the existing callback-gate,
route/Encoding/session/capability, Session-admission, and application path. Durable application
includes the write-once read/compare/write decision and the terminal engine outcome. Ephemeral
application includes route, Encoding, active-Session, and capability admission; it proves no
retention or peer delivery. The marker is serialized on the same receiver dispatch/application
boundary. It claims its token under ADR-0028 and creates exactly one immutable `AckResultV1` for the
matching SID, class, and Publisher lane. Another StorageNode, Session, class, or Publisher cannot
contribute to or complete that result.

`applied` proves only the target processing point. `synced` is valid only for durable buffers and,
after all covered application points, invokes the durability barrier defined exclusively by #105
while preventing later same-Publisher receiver processing from crossing the marker. Fence registry
locks are released before engine or synchronization calls. Until #105 is Accepted and implemented,
`synced` capability is rejected locally before marker submission. #107 owns the public durability
options and receipt mapping.

### Failure aggregation and result mapping

Each receiver lane stores its next expected sequence, highest observed identifiable sequence, and
first failure: valid nonzero sequence or lane-global sentinel, Status, and bounded sanitized message.
The lane-global form participates in first-failure order and applies to every marker through value.
`highest_observed_sequence` is updated before duplicate, gap, malformed-with-sequence, application,
or dispatch-overflow disposition, so rejected later
data cannot disappear from marker-order proof. If no new StorageNode lane can be retained, the
Session/class `unretained_rejection` poison preserves only the weaker but sufficient fact that an
absent lane cannot prove an empty prefix. It retains no per-identity sequence evidence and never
participates in the retained-N or marker-overtake rules. First failure wins. Later failures are counted only in a bounded
diagnostic counter and cannot replace it. A sequence whose processing returned failure still
advances the expected sequence because its terminal processing point is known; a gap or
pre-processing rejection does not.

For StorageNode targets, only remotely observed outcomes become ADR-0028 `AckResultV1`. Fence
results always use `operation_kind = fence`, `applied_count = 0`, `failed_index = UINT32_MAX`, the
requested durability, and the marker's `through_sequence`. A sequence-specific failure uses its
nonzero sequence only when `1 <= N <= through_sequence`; a marker/global synchronization failure
uses `failed_sequence = UINT64_MAX`. For every table row below that names sequence N, a retained
N above the marker prefix is not selected: its higher observation instead uses the marker-overtake
`OutcomeUnknown` row and the sentinel. Here, retained N means per-lane sequence proof; the
Session/class poison retains and names no N. A lane-global or in-prefix first failure is selected before
that row according to the ordered marker algorithm above. Messages use ADR-0028's bounded sanitized UTF-8 rules.

| Observation | Completion path / Status | Durability | `through_sequence` | `failed_sequence` |
|---|---|---|---:|---:|
| local validation/capability rejection before Transport | local exact Status/message/cause; no token or AckResult | N/A | N/A | N/A |
| data Put non-OK after invocation | local `MayHaveSubmitted`; later receiver proof may supersede; no resubmission | N/A | retained for later Fence | N/A |
| all cache sequences complete | direct local waiter `Ok`; no AckResult | local delivery | waiter through | N/A |
| cache decode/internal failure before effect | direct local waiter `Error` | local delivery | waiter through | first sequence N, local only |
| cache failure after possible effect | direct local waiter `OutcomeUnknown` | local delivery | waiter through | first sequence N, local only |
| cache marker unobserved by deadline | local `Timeout` with latest native cause; no AckResult | local delivery | waiter through | N/A |
| empty valid buffer Fence with no crossed positive sequence and no applicable unretained-rejection poison | AckResult `Ok`; no barrier or lane allocation | requested applied or durable synced | 0 | `UINT64_MAX` |
| empty buffer Fence in a poisoned absent-lane Session/class scope | AckResult `OutcomeUnknown`; no barrier or lane allocation | requested applied or durable synced | 0 | `UINT64_MAX` |
| marker overtaken by excluded later data with no selectable lane-global or in-prefix first failure | AckResult `OutcomeUnknown` order violation | requested | marker through | `UINT64_MAX` |
| all buffer sequences applied | AckResult `Ok` | applied | marker through | `UINT64_MAX` |
| durable barrier completes | AckResult `Ok` | synced | marker through | `UINT64_MAX` |
| malformed data with recoverable valid N or stale/duplicate data including dispatch-overflow stale N, when N is within the marker prefix | AckResult `Error` | requested | marker through | N |
| malformed data with recoverable UUID but no valid nonzero sequence, and a retainable lane | AckResult `Error` | requested | marker through | `UINT64_MAX` |
| missing, unprovable, or expected/future valid-data dispatch rejection sequence N, when N is within the marker prefix | AckResult `OutcomeUnknown` | requested | marker through | N |
| positive marker for an absent buffer lane, whether the Session/class scope is poisoned or not | AckResult `OutcomeUnknown` | requested | marker through greater than 0 | 1 |
| missing Session | AckResult `NotFound` | requested | marker through | `UINT64_MAX` |
| Creating/Closing Session | AckResult `InvalidArgument` | requested | marker through | `UINT64_MAX` |
| disabled capability or write-once conflict at N | AckResult `InvalidArgument` | requested | marker through | N |
| engine false/throw after invocation at N | AckResult `OutcomeUnknown` | requested | marker through | N |
| internal exception before engine/sync invocation | AckResult `Error` | requested | marker through | N if attributable, else `UINT64_MAX` |
| durability-barrier failure | AckResult with #105's ADR-0028-allowed Status | synced | marker through | `UINT64_MAX` |
| valid marker malformed but token/route recoverable | AckResult `Error` | requested route value | marker through | `UINT64_MAX` |
| valid token cannot be recovered | no remote result; client can only reach `Timeout` | requested if locally known | local through | N/A |
| receiver disappears, restarts, or evicts result | no observed result; ADR-0028 `Timeout` | requested | local through | N/A |

For a future #105 status outside ADR-0028's closed allowlist, the marker returns `Error`; ADR-0029
does not expand the result schema. A normally proven receiver success supersedes an earlier local
`MayHaveSubmitted` diagnostic for the same covered sequence. Without receiver proof, timeout
preserves the latest local/native cause but never pretends that a remote result existed.

### Bounds, cleanup, and lifecycle

| State collection | Exact bound | Cleanup event |
|---|---:|---|
| sender lane identity/sequence/failure | O(1) per live Publisher | quiescent destruction or Transport close |
| sender Fence waiter/token | 1 per Publisher | result, timeout, cancellation, move-assignment cleanup, or destruction |
| Transport receiver dispatch entries | 256 admitted per Transport; fixed ticket counters | callback return, undeclare, session close, or Stop |
| initiating ParamCache receiver lane | 1 per Attach generation | Detach, move-assignment cleanup, destruction, or Transport close |
| receiver sequence proof | O(1) per lane: next expected, highest observed, first failure | lane cleanup only |
| unretained StorageNode rejection proof | 1 poison bit per active Session/class | matching CloseSession or Stop |
| ParamCache local Fence waiter | 1 | completion, timeout, Detach, or destruction |
| StorageNode buffer Publisher lanes | 4096 node-wide | matching CloseSession, Stop, or node destruction |
| Processing Fence token | 1 per Publisher lane | immutable completion or StorageNode Stop |
| completed ACK results | existing ADR-0028 node-wide 4096 ring | completion-order eviction or Stop |
| diagnostics | fixed counters plus existing bounded messages | state cleanup |

StorageNode never evicts an admitted live lane merely to admit a new identity. At the 4096-lane
limit, any candidate for a new attached buffer Publisher is rejected before engine mutation, a
bounded diagnostic is retained, and the matching Session/class `unretained_rejection` bit is set
before the rejection returns. The bit does not retain whether that candidate carried sequence 1,
a future valid sequence, or no recoverable sequence. A later positive marker for any absent lane is
therefore evaluated only as a missing first publication and returns `OutcomeUnknown` with
`failed_sequence = 1`; it does not attribute the result to the rejected candidate. An empty marker
for an absent lane in the poisoned scope returns `OutcomeUnknown` with the sentinel.
Attachment-free ADR-0032 traffic remains unaffected. A crashed Publisher can consume one lane until
CloseSession or Stop, and one rejected identity can conservatively poison empty absent-lane Fences in
that scope for the same lifetime. These liveness costs are accepted to avoid unbounded state,
unsafe identity reuse, or false Fence success.

Detach, ParamCache destination move-assignment cleanup, ParamCache destruction, receiver undeclare,
or Transport close cancels a registered local waiter with `Status::Disconnected`, then quiesces
admitted callbacks and clears the Attach generation. Moving the source ParamCache or Publisher
without concurrent calls transfers its state and does not cancel it; calls on a moved-from Publisher
return `Disconnected`.

CloseSession rejects a buffer marker for an absent Session with `NotFound` and for a Creating or
Closing Session with `InvalidArgument`; both use the failed-sequence sentinel unless a covered data
sequence already owns the first failure. A marker already admitted while Active completes before
CloseSession clears the lane because Close waits for admission quiescence. StorageNode Stop before
marker observation creates no result and the caller reaches `Timeout`. Stop after token claim uses
ADR-0028's completion guard: `Error` before an engine or synchronization call, and
`OutcomeUnknown` after such a call unless the callee proves a stronger result. Stop then clears lane,
Processing, and Completed state.

Undeclare and Transport close stop the bounded dispatch lane before callback-owned state is
released. Receiver disappearance, restart, or an old cache receiver UUID produces no matching
completion and therefore `Timeout`; old tokens, UUIDs, and late callbacks cannot revive after
cleanup. A later Attach, Start, Session recreation, or process restart has new receiver/Publisher
identities and cannot answer an old Fence. After a local cache-lane failure, supported recovery is a
quiescent Detach followed by Attach, which creates new receiver and Publisher UUIDs; an Attach
generation never replaces its one retained lane in place.

The lock/lease order is:

```text
sender:
  Publisher admission -> lane mutex -> waiter publication -> Transport submission sequencer

ParamCache receiver:
  Transport dispatch admission/ticket -> callback lease -> cache sequence mutex -> waiter mutex

StorageNode receiver:
  Transport dispatch admission/ticket -> node callback gate -> subscriber_mutex
  -> session_mutex / Session admission -> fence-lane registry -> ADR-0028 token registry
```

Waiter, fence-lane, and token-registry locks are released before Transport calls, logging, external
callbacks, engine calls, or #105 synchronization. Existing ADR-0032 rules still require releasing
`session_mutex` before engine work. Timeout atomically removes/completes the current waiter. Because
tokens are unique and late callbacks retain receiver-owned state rather than operation-owned stack
state, late delivery can neither revive the call nor complete a replacement waiter. No callback may
access operation-owned state after the operation returns.

### State transitions

Publisher and local-waiter transitions are exact:

| State | Trigger | Retained state | Outcome and cleanup |
|---|---|---|---|
| Publisher open | valid data call | incremented `last_sequence`; optional `MayHaveSubmitted` cause | one data Put; return its local result |
| Publisher open/exhausted | data after `UINT64_MAX` | exhausted flag | definite local `InvalidArgument`; no Put |
| Fence idle | valid Fence | immutable token, waiter, through, deadline | one marker Put; enter pending |
| Fence pending | another Fence | existing waiter unchanged | definite local `InvalidArgument`/`operation_in_progress` |
| Fence pending | valid cache completion or AckResult | decoded completion | completion wins over marker Put error; remove waiter |
| Fence pending | deadline | latest local/native cause | `Timeout`; atomically remove waiter |
| Fence pending | Detach/destruction/Transport close | waiter atomically removed before quiescence | local `Disconnected`; admitted call wakes, then cleanup waits; late token ignored |
| Publisher closing | new call | none | `Disconnected`; no submission |

Receiver and StorageNode transitions are exact:

| State | Trigger | Retained state | Outcome and cleanup |
|---|---|---|---|
| lane absent | sequence 1 and capacity | highest observed 1; next expected 1, then terminal result | create lane, process once, advance to 2 |
| lane absent | sequence above 1 | highest observed incoming sequence; no inferred application | `OutcomeUnknown`, first missing sequence 1 |
| lane absent | marker through above 0 | no inferred application | `OutcomeUnknown`, first missing sequence 1 |
| lane active | expected sequence | update highest observed; first failure if any | process once and advance after terminal processing |
| lane active | stale/duplicate sequence | update highest observed; first failure latch | no reapply; `Error` at stale sequence |
| lane active | future sequence | update highest observed before rejection; first failure latch | no apply; `OutcomeUnknown` at next expected |
| lane identifiable and retainable | malformed data with valid nonzero sequence N | highest observed N; first failure latch | no apply; `Error` at N |
| lane identifiable and retainable | malformed data without valid nonzero sequence | lane-global first failure | no apply; `Error` with failed-sequence sentinel for every through value |
| lane active/failed | first marker | next expected, highest observed, first failure | duplicate result, then global failure, in-prefix failure, overtake, missing prefix, or success in that exact order |
| lane absent | empty first buffer marker and unretained-rejection poison | scope poison bit | `OutcomeUnknown` with failed-sequence sentinel; no barrier or lane allocation |
| lane active/failed | retained duplicate token | immutable ADR-0028 result | return original result before current-state order checks |
| marker token absent | valid buffer marker | `Processing` token and fingerprint | complete exactly once to immutable AckResult |
| token Processing | duplicate marker | original token unchanged | no second processing; query has no result yet |
| token Completed | same fingerprint | immutable result | return existing result; no repeated apply/sync |
| token Completed | different fingerprint | original result | collision rejection and bounded diagnostic |
| token evicted | late duplicate | no retained identity guarantee | ADR-0028 permits fresh observation; no resubmission by supported client |
| dispatch full | identifiable valid covered data with retainable lane proof | highest observation plus normal stale/expected/future failure disposition updated synchronously | data unprocessed; later first marker cannot succeed across the rejection |
| dispatch full | malformed candidate with retainable lane proof | malformed sequence-specific or lane-global `Error` disposition updated synchronously | data unprocessed; later first marker cannot succeed across the rejection |
| new-lane registry full, with or without dispatch capacity | identifiable covered data for absent buffer lane | Session/class poison plus bounded diagnostic; no UUID or sequence retained | data unprocessed; positive absent-lane marker fails at missing 1, empty marker uses sentinel |
| dispatch full | marker callback | bounded diagnostic only | marker unobserved; caller reaches `Timeout` |

Lifecycle transitions are exact:

| Trigger | Admission/result behavior | Cleanup |
|---|---|---|
| cache Detach or destination cleanup | reject new callbacks; waiter `Disconnected` | quiesce, clear one lane and receiver UUID |
| receiver undeclare/Transport close | reject dispatch; waiter `Disconnected` | quiesce 256-entry lane and callback state |
| CloseSession from Active | admitted marker finishes; new marker sees Closing `InvalidArgument` | quiesce then remove matching buffer lanes and Session/class poison bits |
| StorageNode Stop before marker observation | no AckResult; caller `Timeout` | quiesce then clear lane/token/result state |
| StorageNode Stop after token claim | admitted callback publishes exactly one immutable `Error` or `OutcomeUnknown` by invocation boundary | Stop quiesces it, then clears state; client may still time out before observing it |
| process/receiver restart | old UUID/token has no state; caller `Timeout` | new generation starts empty |
| late callback after cleanup | admission fails; no completion | cannot access operation-owned state |

### Qualification owned by Issue #158

Issue #158 must first use deterministic fake-Transport tests for concurrent Put/PutBatch/Push/Fence
linearization, empty prefix before and after sequence 1, maximum sequence, overflow, gap, reorder,
marker through 7 after completed sequence 8, marker through N after rejected sequence N+2, dispatch
overflow for existing-lane stale, expected, and future sequences followed by markers through N and
N-1, overlaps between an in-prefix first failure and a higher completed/observed sequence, retained
token duplicate after later data, synchronized marker overtake, duplicate, detected same-UUID
stale/gap conflicts, malformed candidates with a recoverable sequence, UUID only, or neither,
including dispatch-overflow, retainable, and capacity-poison paths, forced same-UUID next-expected
raw injection that confirms the documented indistinguishability and non-authentication boundary,
synchronous loopback, multiple Publisher isolation under distinct generated UUIDs and fixed receiver
binding, one-pending-Fence rejection,
every failure-matrix row, dispatch and 4096-lane limits, new-lane sequence-1, future-sequence, and
UUID-only rejections with registry-only exhaustion and with both limits exhausted, followed by
empty, positive-below-observation, and positive through markers that verify poison never names N,
poison isolation and lifecycle cleanup, attachment-free
dispatch bypass, timeout/late non-revival, no resubmission, and
Detach/CloseSession/Stop/move/destruction quiescence.

Linux and Windows process-isolated Zenoh tests must cover minimum 1.9.0 and the current supported 1.x
version: data and marker on different keys in one session, multiple logical Publishers sharing one
session, distinct publisher sessions, same-session loopback, separate receiver sessions, exact
Block/Data/non-express profile, incompatible QoS rejection, attachment preservation, raw-client
payload transparency, receiver disappearance, and sequence-gap failure. Sanitizers must stress
callback/Fence/Detach/Stop races and bounded-state cleanup. Experiments qualify versions; they do
not weaken the normative sitos proof above.

### Cross-contract ownership

| Authority | Sole responsibility |
|---|---|
| ADR-0028 / #14 | UUID correlation token, `AckAttachmentV1`, `AckResultV1`, token/result ring, ACK query route, total-deadline polling |
| ADR-0029 / #106 | logical lane identity, `FenceLaneAttachmentV1`, marker wire contract, ordering, receiver processing boundaries |
| #158 | production Transport/StorageNode/lane/waiter implementation and executable qualification |
| #99 | ParamCache public local-delivery API and user-facing lifecycle mapping |
| #105 | StorageEngine durability barrier semantics and implementation |
| #107 | BufferPublisher C++/Python APIs, Fence options, receipts, and payload lifetime |
| ADR-0032 / #56 | buffer routes, bare bytes, capability, write-once, and live/persistence separation |
| #108 | retained Session catalog, restart recovery, retention, orphan, and deletion policy |

## Consequences

* Good: under the generated-UUID non-collision condition, UUID and sequence metadata prove Publisher
  isolation and detect gaps without changing user keys, parameter payloads, or ADR-0032 buffer bytes.
* Good: direct cache completion and StorageNode AckResult completion prove the intended receiver
  rather than a sender queue drain or unrelated acknowledgement.
* Good: ADR-0028 remains the only Fence token, result, retention, query, and polling protocol.
* Good: every collection is bounded and every lifecycle path is callback-quiescent.
* Bad: every covered data sample carries 25 extra bytes, and strict FIFO/Block processing can add
  backpressure and head-of-line latency.
* Bad: a gap, duplicate, unsupported QoS profile, lane-cap exhaustion, or Publisher crash fails
  closed and can require a new Publisher identity or Session lifecycle cleanup.
* Bad: a receiver cannot distinguish an accidental generated UUID collision, or raw traffic using the
  same UUID and next valid sequence, from the intended Publisher. UUIDv4 makes the supported-caller
  risk collision-resistant rather than impossible; Fence is conditional on non-collision and is not
  authentication.
* Bad: v1 cannot combine per-data ADR-0028 acknowledgement and Fence-lane metadata on one sample.
* Neutral: Fence is ordering evidence, not authentication, exactly-once delivery, a peer barrier,
  or automatic retry.
* Neutral: `docs/09_dependency_policy.md` attachment-summary reconciliation is intentionally
  separate Issue #160 because #106's six-file scope excludes that document.

## Options Considered

* **One-shot session puts** — selected because one logical Publisher must write varying data keys
  and reserved marker keys through one Transport generation.
* **Declared Zenoh Publisher as the identity** — rejected because its key expression is fixed and
  stable subscriber samples do not expose a suitable cross-key identity.
* **Sitos-serialized logical Publisher lanes** — selected because the mutex supplies a precise
  local linearization point while UUID/sequence evidence supplies receiver proof.
* **One session per logical Publisher** — rejected as mandatory topology because it conflicts with
  shared/injected-session use and still does not expose a stable receiver identity; distinct
  sessions remain supported for distinct Publishers.
* **Session identity as Publisher identity** — rejected because unrelated logical Publishers may
  share a session and no stable identity is exposed through the Transport abstraction.
* **Generated logical UUID** — selected because it is transport-independent, move-stable, and
  probabilistically restart-distinct; collision resistance is sufficient for the supported-caller
  identity condition but is not an absolute uniqueness guarantee.
* **Attachment-carried lane metadata** — selected because payload-carried metadata would change
  parameter v1 or opaque buffer values, and key-carried metadata would change user routes.
* **Unstable Zenoh SourceInfo/Publisher ids** — rejected by the dependency isolation policy and the
  supported 1.x compatibility boundary.
* **All marker fields in the payload on one fixed `meta/fence` key** — rejected because a malformed
  payload would prevent a StorageNode from truthfully constructing required durability/through
  fields for a recoverable token; the selected route carries those fields and the payload versions
  the envelope.
* **Inline buffer `:fence` route** — rejected because ADR-0032 reserves buffer routes for opaque
  values and prohibits inline control operations.
* **Reuse `meta/ack/<uuid>` as the marker route** — rejected because that route is a read-only
  result query surface; mixing submission and result roles would overlap ADR-0028 and obscure the
  target and Publisher lane.
* **One shared target-aware marker with target-specific routes** — selected over unrelated formats;
  receiver identity and StorageNode durability require distinct route fields, while all wire and
  sequencing rules remain common.
* **Result-query completion for the initiating cache** — rejected because it would prove
  StorageNode processing rather than the initiating subscriber. A direct token waiter is selected.
* **Ticket/sequence-only sender linearization** — rejected because concurrent calls still need an
  exact admission order. A lane mutex selects the prefix; sequence proves it remotely.
* **First-failure latch** — selected because it is deterministic, O(1), and maps directly to
  `failed_sequence`. Retaining every failure is unbounded, and last-failure wins would hide the
  earliest broken prefix.
* **Local sender queue drain** — rejected because it proves neither callback invocation nor
  receiver application.
* **Unrelated StorageNode acknowledgement for local cache delivery** — rejected because it proves
  the wrong receiver.
* **Separate Fence ACK attachment/result/route** — rejected because ADR-0028 already owns those
  mechanisms.
* **One global Publisher lane** — rejected because unrelated Publishers would share failures and
  ordering.
* **Automatic per-value Fence** — rejected because #107 requires an explicit application boundary
  and the overhead would be unconditional.
* **Data or marker resubmission** — rejected because effects can be duplicated and ADR-0028 defines
  one-submit semantics.

## References

* Issue #106 and production successor #158
* Consumer Issues #99 and #107; durability Issue #105; restart Issue #108
* Dependency-policy reconciliation Issue #160
* [ADR-0028](0028-unify-acknowledged-operation-results.md)
* [ADR-0032](0032-mixed-session-buffer-routes.md)
* [Architecture](../02_architecture.md), [wire protocol](../03_wire_protocol.md),
  [contract registry](../08_contract_registry.md), and
  [Zenoh dependency policy](../09_dependency_policy.md) §§2–4
* [Zenoh 1.9.0 reliability and congestion-control definitions](https://github.com/eclipse-zenoh/zenoh/blob/1.9.0/commons/zenoh-protocol/src/core/mod.rs)
* [Zenoh 1.9.0 publication/QoS builder](https://github.com/eclipse-zenoh/zenoh/blob/1.9.0/zenoh/src/api/builders/publisher.rs)
* [Zenoh-c 1.9.0 option and attachment definitions](https://github.com/eclipse-zenoh/zenoh-c/blob/1.9.0/include/zenoh_commons.h)
* [Zenoh-c 1.9.0 publisher implementation](https://github.com/eclipse-zenoh/zenoh-c/blob/1.9.0/src/publisher.rs)
