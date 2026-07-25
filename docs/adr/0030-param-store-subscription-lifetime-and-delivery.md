# ADR-0030: Define ParamStore subscription lifetime and delivery semantics

## Status

Proposed — Issue #16

## Context

`ParamStore::Subscribe` needs an application-facing callback API over Transport subscriber samples.
A callback can overlap declaration, other subscriber callbacks, and synchronous write self-echoes.
Low-level Transport handle destruction alone does not define the lifetime of queued work, user
callbacks, diagnostics, or callback-owned state.

## Decision

Issue #16 adds a move-only `ParamSubscription` that owns its Transport subscription, callback state,
and client LogSink lifetime independently of `ParamStore`. `Subscribe` stages owned samples during
subscriber declaration and drains them only after declaration succeeds. Declaration failure invokes
no user callback.

Each subscription uses a threadless single-flight drainer. Callbacks are serialized per subscription,
full batches are one non-interleaved work item, duplicates and encoded order are preserved, and
non-drainer native callbacks wait for their work. A synchronous callback on the drainer thread queues
reentrant writes without waiting. There is no global order, self-echo suppression, or duplicate
suppression.

`Close()` is synchronous, idempotent, and `noexcept`: it closes admission, undeclares the native
subscriber, waits for admitted native callbacks and queued/draining work, and guarantees that no user
callback or LogSink call remains or starts after return. Self-close, self-destruction, and self-move
from a callback or subscription-owned LogSink are unsupported.

Callbacks observe valid Transport delivery and decoding only. They do not acknowledge StorageNode
application, persistence, visibility, or peer delivery. Python callback dispatch remains Issue #26.

## Consequences

- Applications receive owned relative keys and values through an explicit PUT/DELETE event type.
- Callback exceptions are contained and logged through the injected backend-neutral LogSink.
- The API has no worker thread, timeout cancellation, queue capacity, or overflow policy.
- Native subscriber closure lifetime must be proven by the Issue #16 prerequisite regression; an
  unresolved native lifetime failure blocks implementation and requires a separate Transport fix.
- This ADR does not add a wire surface or stable identifier and reuses existing key, Encoding,
  payload-v1, and batch-v1 contracts.

## Alternatives considered

- Fire-and-forget callbacks were rejected because Close could return while callbacks or diagnostics
  still accessed subscription-owned state.
- A dedicated worker thread was rejected to keep the client API KISS and preserve callback-thread
  flexibility; it is not required for serialized delivery.
- Initial Get/List replay was rejected because this API is delta-only; ParamCache owns snapshot
  attachment semantics.
