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
#include <optional>
#include <string>
#include <string_view>

#include "sitos/transport.hpp"

#include "zenoh_transport_test_access.hpp"

namespace sitos {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Error codes for conditions zenoh-c does not distinguish. These are
// negative so they never collide with z_result_t values (>= 0).
//
// NOTE: full migration to the project "Status" enum (docs/04_api_cpp.md §1)
// is tracked separately; these named sentinels are an interim improvement
// over the bare MakeError(-1) magic number.
enum class TransportErrc {
  kErrDisconnected = -1,  // session not opened or already closed
  kErrInvalidArg = -2,    // invalid argument (e.g. negative timeout)
  kErrNoQuery = -3,       // queryable destroyed or query unavailable
};

// A custom error_category so std::error_code::message() produces meaningful
// text instead of the platform-dependent generic_category() strings.
class ZenohErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override { return "sitos.zenoh"; }

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
        if (ev == Z_OK) return "ok";
        return "zenoh error code " + std::to_string(ev);
    }
  }
};

const std::error_category& ZenohCategory() {
  static const ZenohErrorCategory kCategory;
  return kCategory;
}

std::error_code MakeError(z_result_t rc) {
  if (rc == Z_OK) return {};
  return {static_cast<int>(rc), ZenohCategory()};
}

std::error_code MakeError(TransportErrc ec) { return {static_cast<int>(ec), ZenohCategory()}; }

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

// Build a z_owned_keyexpr_t from a string. Returns an error Result if zenoh
// rejects the key.
Result<ZenohOwned<z_owned_keyexpr_t>> MakeKeyexpr(std::string_view key) {
  ZenohOwned<z_owned_keyexpr_t> ke;
  z_result_t rc = z_keyexpr_from_str(ke.get(), std::string(key).c_str());
  if (rc != Z_OK) return Result<ZenohOwned<z_owned_keyexpr_t>>::Err(MakeError(rc));
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
      return Result<ZenohOwned<z_owned_encoding_t>>::Err(MakeError(rc));
    }
    return Result<ZenohOwned<z_owned_encoding_t>>::Ok(std::move(z_enc));
  }

  z_result_t rc = z_encoding_from_substr(z_enc.get(), enc.id.data(), enc.id.size());
  if (rc != Z_OK) {
    return Result<ZenohOwned<z_owned_encoding_t>>::Err(MakeError(rc));
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
    return Result<ZenohOwned<z_owned_bytes_t>>::Err(MakeError(rc));
  }
  p.mark_valid();
  return Result<ZenohOwned<z_owned_bytes_t>>::Ok(std::move(p));
}

struct GetReplyCtx {
  explicit GetReplyCtx(const Transport::QueryResultSink& result_sink) : sink(result_sink) {}

  Transport::QueryResultSink sink;
  std::atomic<bool> stop{false};
};

void OnGetReply(z_loaned_reply_t* reply, void* context) {
  auto* c = static_cast<GetReplyCtx*>(context);
  try {
    if (c->stop.load(std::memory_order_relaxed)) return;

    const z_loaned_sample_t* sample = z_reply_ok(reply);
    if (!sample) return;

    const z_loaned_bytes_t* payload = z_sample_payload(sample);
    ZenohOwned<z_owned_slice_t> slice;
    if (z_bytes_to_slice(payload, slice.get()) != Z_OK) return;
    slice.mark_valid();

    const auto* data = reinterpret_cast<const std::byte*>(z_slice_data(slice.loan()));
    auto len = z_slice_len(slice.loan());

    z_view_string_t ks;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
    std::string key_str(z_string_data(z_view_string_loan(&ks)),
                        z_string_len(z_view_string_loan(&ks)));

    Encoding enc = ReadEncoding(sample);

    if (c->sink && !c->sink(key_str, std::span<const std::byte>(data, len), enc)) {
      c->stop.store(true, std::memory_order_relaxed);
    }
  } catch (...) {
    // Contain all C++ exceptions at the zenoh-c callback boundary (#67).
    // The RAII slice owner drops a successfully-created slice while unwinding.
    c->stop.store(true, std::memory_order_relaxed);
  }
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
    return Result<void>::Err(MakeError(TransportErrc::kErrNoQuery));
  }

  auto enc = MakeEncoding(encoding);
  if (!enc.IsOk()) return Result<void>::Err(enc.Error());

  auto ke = MakeKeyexpr(key);
  if (!ke.IsOk()) return Result<void>::Err(ke.Error());

  auto p = MakeBytes(payload);
  if (!p.IsOk()) return Result<void>::Err(p.Error());

  auto callback_state = impl_->callback_state;
  auto queryable = callback_state->queryable;
  if (!queryable) return Result<void>::Err(MakeError(TransportErrc::kErrNoQuery));

  // Hold the lock through z_query_reply() so Queryable::~Queryable() cannot
  // drop the underlying queryable while zenoh uses the loaned query.
  std::lock_guard<std::mutex> lock(queryable->mutex);
  if (!callback_state->active.load() || !queryable->alive || !callback_state->query) {
    return Result<void>::Err(MakeError(TransportErrc::kErrNoQuery));
  }

  z_query_reply_options_t opts;
  z_query_reply_options_default(&opts);
  opts.encoding = enc.Value().moved();

  z_result_t rc = z_query_reply(callback_state->query, ke.Value().loan(), p.Value().moved(), &opts);

  if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
  return Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

struct Subscription::Impl {
  z_owned_subscriber_t subscriber;
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
  delete static_cast<TestSubscriberContext*>(context);
}

struct TestSubscriberSession {
  z_owned_session_t session{};
  bool valid = false;

  TestSubscriberSession() {
    z_owned_config_t config;
    if (z_config_default(&config) != Z_OK) return;
    valid = z_open(&session, z_move(config), nullptr) == Z_OK;
  }

  ~TestSubscriberSession() {
    if (valid) z_close(z_session_loan_mut(&session), nullptr);
    z_drop(z_move(session));
  }
};

TestSubscriberSession& GetTestSubscriberSession() {
  static TestSubscriberSession session;
  return session;
}

}  // namespace

Subscription::Subscription() = default;
Subscription::~Subscription() { Reset(); }
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void Subscription::Reset() noexcept {
  if (!impl_) return;
  z_drop(z_move(impl_->subscriber));
  impl_.reset();
}

namespace transport_test_access {

bool SubscriptionTestAccess::IsAvailable() { return GetTestSubscriberSession().valid; }

Subscription SubscriptionTestAccess::Make(std::string_view keyexpr,
                                          std::function<void()> callback) {
  Subscription subscription;
  auto& test_session = GetTestSubscriberSession();
  if (!test_session.valid) return subscription;

  auto ke = MakeKeyexpr(keyexpr);
  if (!ke.IsOk()) return subscription;

  auto* callback_context = new TestSubscriberContext{std::move(callback)};
  z_owned_closure_sample_t closure;
  z_closure_sample(&closure, OnTestSubscriberSample, DropTestSubscriberContext,
                   callback_context);

  z_owned_subscriber_t subscriber{};
  z_subscriber_options_t options;
  z_subscriber_options_default(&options);
  if (z_declare_subscriber(z_session_loan(&test_session.session), &subscriber,
                           ke.Value().loan(), z_move(closure), &options) != Z_OK) {
    return subscription;
  }

  subscription.impl_ = std::make_unique<Subscription::Impl>();
  z_subscriber_take(&subscription.impl_->subscriber, z_move(subscriber));
  return subscription;
}

bool SubscriptionTestAccess::Publish(std::string_view keyexpr) {
  auto& test_session = GetTestSubscriberSession();
  if (!test_session.valid) return false;

  auto ke = MakeKeyexpr(keyexpr);
  if (!ke.IsOk()) return false;
  const std::byte payload = std::byte{0x01};
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

Queryable::Queryable() = default;
Queryable::~Queryable() { Reset(); }

void Queryable::Reset() noexcept {
  if (!impl_) return;

  auto state = impl_->state;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->alive = false;
    z_drop(z_move(state->queryable));
  }
  impl_.reset();
}

Queryable::Queryable(Queryable&&) noexcept = default;
Queryable& Queryable::operator=(Queryable&& other) noexcept {
  if (this != &other) {
    Reset();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

// ---------------------------------------------------------------------------
// ZenohTransport
// ---------------------------------------------------------------------------

namespace {

bool OpenZenohSession(z_owned_session_t* session) {
  z_owned_config_t config;
  if (z_config_default(&config) != Z_OK) return false;
  return z_open(session, z_move(config), nullptr) == Z_OK;
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
  ZenohTransport() : session_valid_(OpenZenohSession(&session_)) {}

  bool IsSessionValid() const { return session_valid_; }

  ~ZenohTransport() override {
    if (session_valid_) {
      z_close(z_session_loan_mut(&session_), nullptr);
    }
    z_drop(z_move(session_));
  }

  Result<void> Put(std::string_view key, std::span<const std::byte> payload, Encoding encoding,
                   PutOptions /*options*/) override {
    if (!session_valid_) return Result<void>::Err(MakeError(TransportErrc::kErrDisconnected));

    auto enc = MakeEncoding(encoding);
    if (!enc.IsOk()) return Result<void>::Err(enc.Error());

    auto ke = MakeKeyexpr(key);
    if (!ke.IsOk()) return Result<void>::Err(ke.Error());

    auto p = MakeBytes(payload);
    if (!p.IsOk()) return Result<void>::Err(p.Error());

    z_put_options_t opts;
    z_put_options_default(&opts);
    opts.encoding = enc.Value().moved();

    z_result_t rc = z_put(z_session_loan(&session_), ke.Value().loan(), p.Value().moved(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Delete(std::string_view key, PutOptions /*options*/) override {
    if (!session_valid_) return Result<void>::Err(MakeError(TransportErrc::kErrDisconnected));

    auto ke = MakeKeyexpr(key);
    if (!ke.IsOk()) return Result<void>::Err(ke.Error());

    z_delete_options_t opts;
    z_delete_options_default(&opts);

    z_result_t rc = z_delete(z_session_loan(&session_), ke.Value().loan(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds timeout) override {
    if (!session_valid_) return Result<void>::Err(MakeError(TransportErrc::kErrDisconnected));
    if (timeout.count() < 0) return Result<void>::Err(MakeError(TransportErrc::kErrInvalidArg));

    auto ke = MakeKeyexpr(keyexpr);
    if (!ke.IsOk()) return Result<void>::Err(ke.Error());

    auto* reply_ctx = new GetReplyCtx(sink);

    z_owned_closure_reply_t closure;
    z_closure_reply(
        &closure, OnGetReply, +[](void* context) { delete static_cast<GetReplyCtx*>(context); },
        reply_ctx);

    z_get_options_t opts;
    z_get_options_default(&opts);
    opts.timeout_ms = static_cast<uint64_t>(timeout.count());

    z_result_t rc = z_get(z_session_loan(&session_), ke.Value().loan(), "", z_move(closure), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Subscription DeclareSubscriber(
      std::string_view /*keyexpr*/,
      std::function<void(const TransportSample&)> /*callback*/) override {
    // TODO(#3): implement subscriber — currently a stub that silently discards
    // both keyexpr and callback. The caller receives an empty Subscription
    // (impl_ == nullptr) that never delivers samples.
    return {};
  }

  Queryable DeclareQueryable(std::string_view keyexpr_str,
                             const std::function<void(TransportQuery&)>& callback) override {
    Queryable q;
    // An empty callback is a programming error; return an empty handle with
    // defined, safe behavior rather than registering a closure that would
    // throw std::bad_function_call on every query (#67).
    if (!callback) return q;
    q.impl_ = std::make_unique<Queryable::Impl>();
    if (!session_valid_) {
      q.impl_.reset();
      return q;
    }

    q.impl_->state->callback = callback;

    auto ke = MakeKeyexpr(keyexpr_str);
    if (!ke.IsOk()) {
      q.impl_.reset();
      return q;
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
      q.impl_.reset();
    }
    return q;
  }

 private:
  z_owned_session_t session_{};
  bool session_valid_ = false;
};

}  // namespace sitos

std::unique_ptr<sitos::Transport> sitos::MakeZenohTransport() {
  auto t = std::make_unique<sitos::ZenohTransport>();
  if (!t->IsSessionValid()) return nullptr;
  return t;
}
