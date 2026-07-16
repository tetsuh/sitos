// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// ZenohTransport — wraps the zenoh C API behind the Transport abstraction.
// All raw zenoh-c headers and types are confined to this translation unit.

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <zenoh.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "sitos/transport.hpp"

#include "config_failure.hpp"
#include "declaration_handle_lifecycle.hpp"
#include "get_completion.hpp"
#include "zenoh_transport_test_access.hpp"

namespace sitos {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Adapter-defined error codes for conditions zenoh-c does not distinguish.
// Semantic sentinel branches classify these codes explicitly into Status while
// retaining them as the diagnostic cause, rather than using bare magic numbers.
enum class TransportErrc {
  kErrDisconnected = -1,  // session not opened or already closed
  kErrInvalidArg = -2,    // invalid argument (e.g. negative timeout)
  kErrNoQuery = -3,       // queryable destroyed or query unavailable
};

static_assert(!std::is_convertible_v<TransportErrc, z_result_t>);
static_assert(!std::is_convertible_v<z_result_t, TransportErrc>);

// Separate categories keep adapter-generated diagnostics distinct from native
// zenoh-c results even when their numeric values are identical.
class ZenohErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override { return "sitos.zenoh"; }

  std::string message(int ev) const override {
    if (ev == Z_OK) return "ok";
    return "zenoh error code " + std::to_string(ev);
  }
};

class TransportErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override { return "sitos.transport"; }

  std::string message(int ev) const override {
    using enum TransportErrc;
    switch (static_cast<TransportErrc>(ev)) {
      case kErrDisconnected:
        return "zenoh session is not available";
      case kErrInvalidArg:
        return "invalid argument";
      case kErrNoQuery:
        return "query is no longer valid (queryable destroyed)";
      default:
        return "transport error code " + std::to_string(ev);
    }
  }
};

const std::error_category& ZenohCategory() {
  static const ZenohErrorCategory kCategory;
  return kCategory;
}

const std::error_category& TransportCategory() {
  static const TransportErrorCategory kCategory;
  return kCategory;
}

std::error_code MakeZenohError(z_result_t rc) {
  if (rc == Z_OK) return {};
  return {static_cast<int>(rc), ZenohCategory()};
}

std::error_code MakeTransportError(TransportErrc ec) {
  return {static_cast<int>(ec), TransportCategory()};
}

template <typename T>
Result<T> SemanticTransportError(Status status, TransportErrc code) {
  const auto cause = MakeTransportError(code);
  return Result<T>::Err(status, cause.message(), cause);
}

// RAII wrapper for any z_owned_*_t. Calls z_drop(z_move(obj)) on destruction
// and on close of the owning scope, removing the manual z_drop chains that
// were previously duplicated across Put()/Reply(). Move-only; copying is
// deleted to preserve single ownership.
template <typename T>
class ZenohOwned {
 public:
  ZenohOwned() = default;
  explicit ZenohOwned(T obj) : obj_(obj) {}

  ZenohOwned(const ZenohOwned&) = delete;
  ZenohOwned& operator=(const ZenohOwned&) = delete;

  ZenohOwned(ZenohOwned&& other) noexcept : obj_(other.obj_), moved_(other.moved_) {
    other.moved_ = true;
  }
  ZenohOwned& operator=(ZenohOwned&& other) noexcept {
    if (this != &other) {
      Drop();
      obj_ = other.obj_;
      moved_ = other.moved_;
      other.moved_ = true;
    }
    return *this;
  }

  ~ZenohOwned() { Drop(); }

  // Lender: returns a pointer suitable for z_move(obj_out) output parameters.
  T* get() { return &obj_; }

  // Transfers ownership to an API that takes z_moved_*_t*.
  auto moved() {
    moved_ = true;
    return z_move(obj_);
  }

  // Loan for APIs that take z_loaned_*_t*.
  auto loan() { return z_loan(obj_); }

  // Mutable loan for APIs that update an initialized owned value.
  auto loan_mut() { return z_loan_mut(obj_); }

  explicit operator bool() const { return !moved_; }

  // Called by helpers after a successful z_*_from_* populates obj_.
  void mark_valid() { moved_ = false; }

 private:
  void Drop() {
    if (!moved_) z_drop(z_move(obj_));
    moved_ = true;
  }

  T obj_{};
  bool moved_ = true;  // default-constructed state is "empty/moved-from"
};

// T invokes transfer from its constructor, after make_unique has allocated storage.
template <typename T, typename Transfer>
std::unique_ptr<T> MakeUniqueWithDeferredTransfer(Transfer&& transfer) {
  return std::make_unique<T>(std::forward<Transfer>(transfer));
}

class AllocationFailureProbe {
 public:
  template <typename Transfer>
  explicit AllocationFailureProbe(Transfer&& transfer) {
    std::invoke(std::forward<Transfer>(transfer));
  }

  static void* operator new(std::size_t) { throw std::bad_alloc(); }
};

// Build a z_owned_keyexpr_t from a string. Returns an error Result if zenoh
// rejects the key.
Result<ZenohOwned<z_owned_keyexpr_t>> MakeKeyexpr(std::string_view key) {
  ZenohOwned<z_owned_keyexpr_t> ke;
  z_result_t rc = z_keyexpr_from_str(ke.get(), std::string(key).c_str());
  if (rc != Z_OK) return Result<ZenohOwned<z_owned_keyexpr_t>>::Err(MakeZenohError(rc));
  ke.mark_valid();  // ownership acquired
  return Result<ZenohOwned<z_owned_keyexpr_t>>::Ok(std::move(ke));
}

bool IsSitosSchema(std::string_view id) {
  return id == Encoding::kSitosV1 || id == Encoding::kSitosV1Batch;
}

std::optional<std::string_view> SitosSchemaFromEncoding(std::string_view id) {
  constexpr std::string_view kCanonicalBytesPrefix = "zenoh/bytes;";
  constexpr std::string_view kLegacyBytesPrefix = "zenoh.bytes;";
  if (id.starts_with(kCanonicalBytesPrefix)) {
    id.remove_prefix(kCanonicalBytesPrefix.size());
  } else if (id.starts_with(kLegacyBytesPrefix)) {
    id.remove_prefix(kLegacyBytesPrefix.size());
  }
  if (IsSitosSchema(id)) return id;
  return std::nullopt;
}

// Build a z_owned_encoding_t from a transport-independent Encoding id.
Result<ZenohOwned<z_owned_encoding_t>> MakeEncoding(const Encoding& enc) {
  ZenohOwned<z_owned_encoding_t> z_enc;
  if (auto schema = SitosSchemaFromEncoding(enc.id); schema.has_value()) {
    z_encoding_clone(z_enc.get(), z_encoding_zenoh_bytes());
    z_enc.mark_valid();
    z_result_t rc = z_encoding_set_schema_from_substr(z_enc.loan_mut(), schema->data(),
                                                       schema->size());
    if (rc != Z_OK) {
      return Result<ZenohOwned<z_owned_encoding_t>>::Err(MakeZenohError(rc));
    }
    return Result<ZenohOwned<z_owned_encoding_t>>::Ok(std::move(z_enc));
  }

  z_result_t rc = z_encoding_from_substr(z_enc.get(), enc.id.data(), enc.id.size());
  if (rc != Z_OK) {
    return Result<ZenohOwned<z_owned_encoding_t>>::Err(MakeZenohError(rc));
  }
  z_enc.mark_valid();
  return Result<ZenohOwned<z_owned_encoding_t>>::Ok(std::move(z_enc));
}

Encoding NormalizeEncoding(std::string wire_id) {
  if (auto schema = SitosSchemaFromEncoding(wire_id); schema.has_value()) {
    return Encoding{std::string(*schema)};
  }
  return Encoding{std::move(wire_id)};
}

Encoding ReadEncoding(const z_loaned_sample_t* sample) {
  ZenohOwned<z_owned_string_t> wire;
  z_encoding_to_string(z_sample_encoding(sample), wire.get());
  wire.mark_valid();
  return NormalizeEncoding(
      std::string(z_string_data(wire.loan()), z_string_len(wire.loan())));
}

// Build a z_owned_bytes_t from a byte payload.
Result<ZenohOwned<z_owned_bytes_t>> MakeBytes(std::span<const std::byte> payload) {
  ZenohOwned<z_owned_bytes_t> p;
  z_result_t rc = z_bytes_copy_from_buf(p.get(), reinterpret_cast<const uint8_t*>(payload.data()),
                                        payload.size());
  if (rc != Z_OK) {
    return Result<ZenohOwned<z_owned_bytes_t>>::Err(MakeZenohError(rc));
  }
  p.mark_valid();
  return Result<ZenohOwned<z_owned_bytes_t>>::Ok(std::move(p));
}

Result<transport_internal::QueryReply> ConvertGetReply(z_loaned_reply_t* reply) {
  if (!z_reply_is_ok(reply)) {
    return Result<transport_internal::QueryReply>::Err(
        Status::Error, "zenoh get reply reported an error", MakeErrorCode(Status::Error));
  }

  const z_loaned_sample_t* sample = z_reply_ok(reply);
  if (sample == nullptr) {
    return Result<transport_internal::QueryReply>::Err(
        Status::Error, "zenoh get reply did not contain a sample", MakeErrorCode(Status::Error));
  }

  const z_loaned_bytes_t* payload = z_sample_payload(sample);
  if (payload == nullptr) {
    return Result<transport_internal::QueryReply>::Err(
        Status::Error, "zenoh get reply did not contain a payload", MakeErrorCode(Status::Error));
  }
  auto slice = std::make_shared<ZenohOwned<z_owned_slice_t>>();
  const z_result_t slice_result = z_bytes_to_slice(payload, slice->get());
  if (slice_result != Z_OK) {
    return Result<transport_internal::QueryReply>::Err(
        Status::Error, "failed to read zenoh get reply payload", MakeZenohError(slice_result));
  }
  slice->mark_valid();

  z_view_string_t key_view;
  z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key_view);
  std::string key(z_string_data(z_view_string_loan(&key_view)),
                  z_string_len(z_view_string_loan(&key_view)));
  const auto* data = reinterpret_cast<const std::byte*>(z_slice_data(slice->loan()));
  const auto length = z_slice_len(slice->loan());

  return Result<transport_internal::QueryReply>::Ok(
      transport_internal::QueryReply{std::move(key), std::span<const std::byte>(data, length),
                                     ReadEncoding(sample), std::move(slice)});
}

void OnGetReply(z_loaned_reply_t* reply, void* context) noexcept {
  transport_internal::GetCompletion* completion = nullptr;
  try {
    auto* completion_context =
        static_cast<std::shared_ptr<transport_internal::GetCompletion>*>(context);
    auto lease = (*completion_context)->AcquireCallbackLease(completion_context);
    if (!lease.IsEnrolled()) return;
    completion = &lease.Completion();
    completion->ProcessReply([reply] { return ConvertGetReply(reply); });
  } catch (...) {
    // Contain all C++ exceptions at the zenoh-c callback boundary (#67).
    if (completion) completion->RecordCallbackFailure();
  }
}

void DropGetReplyContext(void* context) noexcept {
  std::unique_ptr<std::shared_ptr<transport_internal::GetCompletion>> completion_context(
      static_cast<std::shared_ptr<transport_internal::GetCompletion>*>(context));
  auto completion = std::move(*completion_context);
  if (completion) completion->MarkDropped();
}

void ConfigureGetOptions(z_get_options_t* options, std::chrono::milliseconds timeout) {
  z_get_options_default(options);
  options->consolidation.mode = Z_CONSOLIDATION_MODE_LATEST;
  options->timeout_ms = static_cast<uint64_t>(timeout.count());
}

}  // namespace

namespace transport_test_access {

std::optional<std::string> BuildWireEncoding(const Encoding& encoding) {
  auto result = MakeEncoding(encoding);
  if (!result.IsOk()) return std::nullopt;

  auto owned_encoding = std::move(result.Value());
  ZenohOwned<z_owned_string_t> wire;
  z_encoding_to_string(owned_encoding.loan(), wire.get());
  wire.mark_valid();
  return std::string(z_string_data(wire.loan()), z_string_len(wire.loan()));
}

Encoding NormalizeWireEncoding(std::string wire_encoding) {
  return NormalizeEncoding(std::move(wire_encoding));
}

std::error_code MakeNativeError(std::int8_t code) {
  return MakeZenohError(static_cast<z_result_t>(code));
}

bool AllocationFailurePrecedesOwnershipTransfer() {
  bool ownership_transferred = false;
  try {
    static_cast<void>(MakeUniqueWithDeferredTransfer<AllocationFailureProbe>(
        [&ownership_transferred] { ownership_transferred = true; }));
  } catch (const std::bad_alloc&) {
    return !ownership_transferred;
  }
  return false;
}

bool UsesLatestGetConsolidation() {
  z_get_options_t options;
  ConfigureGetOptions(&options, std::chrono::milliseconds(1));
  return options.consolidation.mode == Z_CONSOLIDATION_MODE_LATEST;
}

}  // namespace transport_test_access

// ---------------------------------------------------------------------------
// TransportQuery
// ---------------------------------------------------------------------------

namespace {

struct QueryableState {
  z_owned_queryable_t queryable;
  std::function<void(TransportQuery&)> callback;
  std::mutex mutex;
  bool alive = true;
};

struct QueryCallbackState {
  const z_loaned_query_t* query = nullptr;
  std::shared_ptr<QueryableState> queryable;
  std::atomic<bool> active{true};
};

}  // namespace

struct TransportQuery::Impl {
  std::shared_ptr<QueryCallbackState> callback_state;
};

TransportQuery::TransportQuery() = default;
TransportQuery::TransportQuery(ReplyHandler handler)
    : test_reply_handler_(std::move(handler)) {}
TransportQuery::~TransportQuery() = default;

Result<void> TransportQuery::Reply(std::string_view key, std::span<const std::byte> payload,
                                   Encoding encoding) {
  if (test_reply_handler_) return test_reply_handler_(key, payload, encoding);

  if (!impl_ || !impl_->callback_state) {
    return SemanticTransportError<void>(Status::Error, TransportErrc::kErrNoQuery);
  }

  auto enc = MakeEncoding(encoding);
  if (!enc.IsOk()) return Result<void>::ErrFrom(enc);

  auto ke = MakeKeyexpr(key);
  if (!ke.IsOk()) return Result<void>::ErrFrom(ke);

  auto p = MakeBytes(payload);
  if (!p.IsOk()) return Result<void>::ErrFrom(p);

  auto callback_state = impl_->callback_state;
  auto queryable = callback_state->queryable;
  if (!queryable) return SemanticTransportError<void>(Status::Error, TransportErrc::kErrNoQuery);

  // Hold the lock through z_query_reply() so Queryable::~Queryable() cannot
  // drop the underlying queryable while zenoh uses the loaned query.
  std::lock_guard<std::mutex> lock(queryable->mutex);
  if (!callback_state->active.load() || !queryable->alive || !callback_state->query) {
    return SemanticTransportError<void>(Status::Error, TransportErrc::kErrNoQuery);
  }

  z_query_reply_options_t opts;
  z_query_reply_options_default(&opts);
  opts.encoding = enc.Value().moved();

  z_result_t rc = z_query_reply(callback_state->query, ke.Value().loan(), p.Value().moved(), &opts);

  if (rc != Z_OK) return Result<void>::Err(MakeZenohError(rc));
  return Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

namespace {

struct SubscriberState {
  explicit SubscriberState(std::function<void(const TransportSample&)> handler)
      : callback(std::move(handler)) {}

  std::function<void(const TransportSample&)> callback;
  std::atomic<bool> active{true};
};

void OnSubscriberSample(z_loaned_sample_t* sample, void* context) noexcept {
  try {
    auto state = *static_cast<std::shared_ptr<SubscriberState>*>(context);
    if (!state || !state->active.load(std::memory_order_acquire)) return;

    const auto kind = z_sample_kind(sample);
    if (kind != Z_SAMPLE_KIND_PUT && kind != Z_SAMPLE_KIND_DELETE) return;

    z_view_string_t key_view;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key_view);
    std::string key(z_string_data(z_view_string_loan(&key_view)),
                    z_string_len(z_view_string_loan(&key_view)));

    if (kind == Z_SAMPLE_KIND_PUT) {
      const z_loaned_bytes_t* payload = z_sample_payload(sample);
      ZenohOwned<z_owned_slice_t> slice;
      if (z_bytes_to_slice(payload, slice.get()) != Z_OK) return;
      slice.mark_valid();
      const auto* data = reinterpret_cast<const std::byte*>(z_slice_data(slice.loan()));
      const auto length = z_slice_len(slice.loan());
      TransportSample transport_sample{
          std::move(key), std::span<const std::byte>(data, length), ReadEncoding(sample),
          std::nullopt, TransportSample::Kind::Put};
      if (state->active.load(std::memory_order_acquire) && state->callback) {
        state->callback(transport_sample);
      }
      return;
    }

    // DELETE payloads and encodings are deliberately not inspected.
    TransportSample transport_sample{std::move(key), {}, {}, std::nullopt,
                                     TransportSample::Kind::Delete};
    if (state->active.load(std::memory_order_acquire) && state->callback) {
      state->callback(transport_sample);
    }
  } catch (...) {
    // Contain all C++ exceptions at the zenoh-c callback boundary.
  }
}

void DropSubscriberContext(void* context) noexcept {
  delete static_cast<std::shared_ptr<SubscriberState>*>(context);
}

}  // namespace

struct Subscription::Impl {
  z_owned_subscriber_t subscriber{};
  std::shared_ptr<SubscriberState> callback_state;
};

namespace {

struct TestSubscriberContext {
  std::function<void()> callback;
};

void OnTestSubscriberSample(z_loaned_sample_t* /*sample*/, void* context) noexcept {
  try {
    auto* state = static_cast<TestSubscriberContext*>(context);
    if (state->callback) state->callback();
  } catch (...) {
    // Keep test-only callbacks contained at the zenoh-c ABI boundary.
  }
}

void DropTestSubscriberContext(void* context) noexcept {
  std::unique_ptr<TestSubscriberContext> state(static_cast<TestSubscriberContext*>(context));
}

struct TestSubscriberSession {
  z_owned_session_t session{};
  bool valid = false;

  TestSubscriberSession() {
    z_owned_config_t config;
    if (z_config_default(&config) != Z_OK) return;
    valid = z_open(&session, z_move(config), nullptr) == Z_OK;
  }

  void Shutdown() noexcept {
    if (!valid) return;
    z_close(z_session_loan_mut(&session), nullptr);
    z_drop(z_move(session));
    valid = false;
  }

  ~TestSubscriberSession() { Shutdown(); }
};

TestSubscriberSession& GetTestSubscriberSession() {
  static TestSubscriberSession session;
  return session;
}

}  // namespace

void Subscription::Reset() noexcept {
  if (impl_) {
    if (impl_->callback_state) {
      impl_->callback_state->active.store(false, std::memory_order_release);
    }
    z_drop(z_move(impl_->subscriber));
    impl_.reset();
  }
  transport_internal::InvokeResetHandler(reset_handler_);
}

namespace transport_test_access {

bool SubscriptionTestAccess::IsAvailable() { return GetTestSubscriberSession().valid; }

void SubscriptionTestAccess::Shutdown() { GetTestSubscriberSession().Shutdown(); }

Subscription SubscriptionTestAccess::Make(std::string_view keyexpr,
                                          std::function<void()> callback) {
  Subscription subscription;
  auto& test_session = GetTestSubscriberSession();
  if (!test_session.valid) return subscription;

  auto ke = MakeKeyexpr(keyexpr);
  if (!ke.IsOk()) return subscription;

  auto impl = std::make_unique<Subscription::Impl>();
  auto callback_context = std::make_unique<TestSubscriberContext>(std::move(callback));
  z_owned_closure_sample_t closure;
  z_closure_sample(&closure, OnTestSubscriberSample, DropTestSubscriberContext,
                   callback_context.release());

  z_subscriber_options_t options;
  z_subscriber_options_default(&options);
  if (z_declare_subscriber(z_session_loan(&test_session.session), &impl->subscriber,
                           ke.Value().loan(), z_move(closure), &options) != Z_OK) {
    subscription.impl_ = std::move(impl);
    subscription.Reset();
    return subscription;
  }

  subscription.impl_ = std::move(impl);
  return subscription;
}

bool SubscriptionTestAccess::Publish(std::string_view keyexpr) {
  auto& test_session = GetTestSubscriberSession();
  if (!test_session.valid) return false;

  auto ke = MakeKeyexpr(keyexpr);
  if (!ke.IsOk()) return false;
  const auto payload = std::byte{0x01};
  auto bytes = MakeBytes(std::span(&payload, 1));
  if (!bytes.IsOk()) return false;
  auto encoding = MakeEncoding(Encoding{std::string(Encoding::kSitosV1)});
  if (!encoding.IsOk()) return false;

  z_put_options_t options;
  z_put_options_default(&options);
  options.encoding = encoding.Value().moved();
  return z_put(z_session_loan(&test_session.session), ke.Value().loan(), bytes.Value().moved(),
               &options) == Z_OK;
}

}  // namespace transport_test_access

// ---------------------------------------------------------------------------
// Queryable
// ---------------------------------------------------------------------------

struct Queryable::Impl {
  std::shared_ptr<QueryableState> state{std::make_shared<QueryableState>()};
};

}  // namespace sitos

// Shared special members require complete Subscription::Impl and Queryable::Impl definitions.
#include "declaration_handle_lifecycle_impl.hpp"

namespace sitos {

void Queryable::Reset() noexcept {
  if (impl_) {
    auto state = impl_->state;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->alive = false;
      z_drop(z_move(state->queryable));
    }
    impl_.reset();
  }
  transport_internal::InvokeResetHandler(reset_handler_);
}

// ---------------------------------------------------------------------------
// ZenohTransport
// ---------------------------------------------------------------------------

namespace {

struct DisconnectedTransportTag {};

z_result_t OpenZenohSession(z_owned_session_t* session, z_moved_config_t* config) {
  return z_open(session, config, nullptr);
}

}  // namespace

class ZenohTransport : public Transport {
 private:
  static void OnQueryable(z_loaned_query_t* query, void* context) {
    std::shared_ptr<QueryCallbackState> callback_state;
    try {
      auto state = *static_cast<std::shared_ptr<QueryableState>*>(context);
      callback_state = std::make_shared<QueryCallbackState>();
      callback_state->query = query;
      callback_state->queryable = state;

      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->alive) {
          callback_state->active.store(false);
          return;
        }
      }

      TransportQuery tq;
      tq.impl_ = std::make_unique<TransportQuery::Impl>();
      tq.impl_->callback_state = callback_state;

      z_view_string_t qks;
      z_keyexpr_as_view_string(z_query_keyexpr(query), &qks);
      tq.keyexpr = std::string(z_string_data(z_view_string_loan(&qks)),
                               z_string_len(z_view_string_loan(&qks)));

      if (state->callback) state->callback(tq);
    } catch (...) {
      // Contain all C++ exceptions at the zenoh-c callback boundary (#67).
      if (callback_state) callback_state->active.store(false);
      return;
    }
    callback_state->active.store(false);
  }

 public:
  explicit ZenohTransport(DisconnectedTransportTag) {}

  template <typename ConfigTransfer>
  explicit ZenohTransport(ConfigTransfer&& config_transfer)
      : open_result_(OpenZenohSession(
            &session_, std::invoke(std::forward<ConfigTransfer>(config_transfer)))),
        session_valid_(open_result_ == Z_OK) {}

  bool IsSessionValid() const { return session_valid_; }
  z_result_t OpenResult() const { return open_result_; }

  ~ZenohTransport() override {
    if (session_valid_) {
      z_close(z_session_loan_mut(&session_), nullptr);
    }
    z_drop(z_move(session_));
  }

  Result<void> Put(std::string_view key, std::span<const std::byte> payload, Encoding encoding,
                   PutOptions /*options*/) override {
    if (!session_valid_) {
      return SemanticTransportError<void>(Status::Disconnected, TransportErrc::kErrDisconnected);
    }

    auto enc = MakeEncoding(encoding);
    if (!enc.IsOk()) return Result<void>::ErrFrom(enc);

    auto ke = MakeKeyexpr(key);
    if (!ke.IsOk()) return Result<void>::ErrFrom(ke);

    auto p = MakeBytes(payload);
    if (!p.IsOk()) return Result<void>::ErrFrom(p);

    z_put_options_t opts;
    z_put_options_default(&opts);
    opts.encoding = enc.Value().moved();

    z_result_t rc = z_put(z_session_loan(&session_), ke.Value().loan(), p.Value().moved(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeZenohError(rc));
    return Result<void>::Ok();
  }

  Result<void> Delete(std::string_view key, PutOptions /*options*/) override {
    if (!session_valid_) {
      return SemanticTransportError<void>(Status::Disconnected, TransportErrc::kErrDisconnected);
    }

    auto ke = MakeKeyexpr(key);
    if (!ke.IsOk()) return Result<void>::ErrFrom(ke);

    z_delete_options_t opts;
    z_delete_options_default(&opts);

    z_result_t rc = z_delete(z_session_loan(&session_), ke.Value().loan(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeZenohError(rc));
    return Result<void>::Ok();
  }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds timeout) override {
    if (timeout.count() <= 0) {
      return SemanticTransportError<void>(Status::InvalidArgument, TransportErrc::kErrInvalidArg);
    }
    if (!session_valid_) {
      return SemanticTransportError<void>(Status::Disconnected, TransportErrc::kErrDisconnected);
    }

    auto ke = MakeKeyexpr(keyexpr);
    if (!ke.IsOk()) return Result<void>::ErrFrom(ke);

    auto completion = std::make_shared<transport_internal::GetCompletion>(sink);
    auto* context = new std::shared_ptr<transport_internal::GetCompletion>(completion);
    z_owned_closure_reply_t closure;
    z_closure_reply(&closure, OnGetReply, DropGetReplyContext, context);

    z_get_options_t opts;
    ConfigureGetOptions(&opts, timeout);

    const z_result_t result =
        z_get(z_session_loan(&session_), ke.Value().loan(), "", z_move(closure), &opts);
    if (result != Z_OK) {
      // z_get consumes the moved closure. Marking completion here is idempotent
      // if zenoh already invoked the drop callback while reporting the failure.
      completion->MarkDropped();
      static_cast<void>(completion->WaitForResult());
      return Result<void>::Err(MakeZenohError(result));
    }
    return completion->WaitForResult();
  }

  Result<Subscription> DeclareSubscriber(
      std::string_view keyexpr_str,
      std::function<void(const TransportSample&)> callback) override {
    if (!session_valid_) {
      return SemanticTransportError<Subscription>(Status::Disconnected,
                                                  TransportErrc::kErrDisconnected);
    }
    if (!callback) {
      return SemanticTransportError<Subscription>(Status::InvalidArgument,
                                                  TransportErrc::kErrInvalidArg);
    }

    auto ke = MakeKeyexpr(keyexpr_str);
    if (!ke.IsOk()) return Result<Subscription>::ErrFrom(ke);
    Subscription subscription;

    subscription.impl_ = std::make_unique<Subscription::Impl>();
    subscription.impl_->callback_state =
        std::make_shared<SubscriberState>(std::move(callback));
    auto* context = new std::shared_ptr<SubscriberState>(subscription.impl_->callback_state);

    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, OnSubscriberSample, DropSubscriberContext, context);

    z_subscriber_options_t options;
    z_subscriber_options_default(&options);
    const z_result_t decl_rc = z_declare_subscriber(
        z_session_loan(&session_), &subscription.impl_->subscriber, ke.Value().loan(),
        z_move(closure), &options);
    if (decl_rc != Z_OK) {
      subscription.Reset();
      return Result<Subscription>::Err(MakeZenohError(decl_rc));
    }
    return Result<Subscription>::Ok(std::move(subscription));
  }

  Result<Queryable> DeclareQueryable(
      std::string_view keyexpr_str,
      std::function<void(TransportQuery&)> callback) override {
    // An empty callback is a programming error; return an error rather than
    // registering a closure that would throw std::bad_function_call (#67).
    if (!callback) {
      return SemanticTransportError<Queryable>(Status::InvalidArgument,
                                               TransportErrc::kErrInvalidArg);
    }
    if (!session_valid_) {
      return SemanticTransportError<Queryable>(Status::Disconnected,
                                               TransportErrc::kErrDisconnected);
    }
    Queryable q;
    q.impl_ = std::make_unique<Queryable::Impl>();

    q.impl_->state->callback = callback;

    auto ke = MakeKeyexpr(keyexpr_str);
    if (!ke.IsOk()) {
      q.Reset();
      return Result<Queryable>::ErrFrom(ke);
    }

    auto* context = new std::shared_ptr<QueryableState>(q.impl_->state);
    z_owned_closure_query_t closure;
    z_closure_query(
        &closure, OnQueryable,
        +[](void* context) { delete static_cast<std::shared_ptr<QueryableState>*>(context); },
        context);

    z_queryable_options_t q_opts;
    z_queryable_options_default(&q_opts);

    z_result_t decl_rc = z_declare_queryable(z_session_loan(&session_), &q.impl_->state->queryable,
                                             ke.Value().loan(), z_move(closure), &q_opts);

    if (decl_rc != Z_OK) {
      q.Reset();
      return Result<Queryable>::Err(MakeZenohError(decl_rc));
    }
    return Result<Queryable>::Ok(std::move(q));
  }

 private:
  z_owned_session_t session_{};
  z_result_t open_result_ = -1;
  bool session_valid_ = false;
};

namespace transport_test_access {

std::unique_ptr<Transport> MakeDisconnectedTransport() {
  return std::make_unique<ZenohTransport>(DisconnectedTransportTag{});
}

}  // namespace transport_test_access

}  // namespace sitos

sitos::Result<std::unique_ptr<sitos::Transport>> sitos::OpenZenohTransport(
    std::optional<std::string_view> config_json) {
  if (config_json.has_value() && config_json->empty()) {
    return Result<std::unique_ptr<Transport>>::Err(Status::InvalidArgument,
                                                   "zenoh configuration must not be empty");
  }

  ZenohOwned<z_owned_config_t> config;
  z_result_t config_result = Z_OK;
  if (config_json.has_value()) {
    config_result =
        zc_config_from_substr(config.get(), config_json->data(), config_json->size());
  } else {
    config_result = z_config_default(config.get());
  }
  if (config_result != Z_OK) {
    const bool user_config_provided = config_json.has_value();
    const auto status = transport_detail::ConfigFailureStatus(user_config_provided);
    const char* message = user_config_provided ? "invalid zenoh configuration"
                                               : "failed to create default zenoh configuration";
    return Result<std::unique_ptr<Transport>>::Err(status, message,
                                                   MakeZenohError(config_result));
  }

  config.mark_valid();
  auto transport = MakeUniqueWithDeferredTransfer<sitos::ZenohTransport>(
      [&config] { return config.moved(); });
  if (!transport->IsSessionValid()) {
    return Result<std::unique_ptr<Transport>>::Err(
        Status::Error, "failed to open zenoh session",
        MakeZenohError(transport->OpenResult()));
  }
  return Result<std::unique_ptr<Transport>>::Ok(std::move(transport));
}

std::unique_ptr<sitos::Transport> sitos::MakeZenohTransport() {
  auto result = OpenZenohTransport();
  if (!result.IsOk()) return nullptr;
  return std::move(result).Value();
}
