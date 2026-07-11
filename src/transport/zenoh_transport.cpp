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

#include "sitos/transport.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

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

std::error_code MakeError(TransportErrc ec) {
  return {static_cast<int>(ec), ZenohCategory()};
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

  ZenohOwned(ZenohOwned&& other) noexcept : obj_(other.obj_) {
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

  // Lender: returns a moved handle for APIs that take z_moved_*_t*.
  auto moved() { return z_move(obj_); }

  // Loan for APIs that take z_loaned_*_t*.
  auto loan() { return z_loan(obj_); }

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

// Build a z_owned_encoding_t from an Encoding id string.
Result<ZenohOwned<z_owned_encoding_t>> MakeEncoding(const Encoding& enc) {
  ZenohOwned<z_owned_encoding_t> z_enc;
  z_result_t rc = z_encoding_from_str(z_enc.get(), enc.id.c_str());
  if (rc != Z_OK) {
    return Result<ZenohOwned<z_owned_encoding_t>>::Err(MakeError(rc));
  }
  z_enc.mark_valid();
  return Result<ZenohOwned<z_owned_encoding_t>>::Ok(std::move(z_enc));
}

// Build a z_owned_bytes_t from a byte payload.
Result<ZenohOwned<z_owned_bytes_t>> MakeBytes(std::span<const std::byte> payload) {
  ZenohOwned<z_owned_bytes_t> p;
  z_result_t rc = z_bytes_copy_from_buf(
      p.get(), reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  if (rc != Z_OK) {
    return Result<ZenohOwned<z_owned_bytes_t>>::Err(MakeError(rc));
  }
  p.mark_valid();
  return Result<ZenohOwned<z_owned_bytes_t>>::Ok(std::move(p));
}

struct GetReplyCtx {
  const Transport::QueryResultSink* sink;
  std::atomic<bool> stop{false};
};

void OnGetReply(z_loaned_reply_t* reply, void* context) {
  auto* c = static_cast<GetReplyCtx*>(context);
  if (c->stop.load(std::memory_order_relaxed)) return;

  const z_loaned_sample_t* sample = z_reply_ok(reply);
  if (!sample) return;

  const z_loaned_bytes_t* payload = z_sample_payload(sample);
  z_owned_slice_t slice;
  z_bytes_to_slice(payload, &slice);

  const auto* data =
      reinterpret_cast<const std::byte*>(z_slice_data(z_slice_loan(&slice)));
  auto len = z_slice_len(z_slice_loan(&slice));

  z_view_string_t ks;
  z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
  std::string key_str(z_string_data(z_view_string_loan(&ks)),
                      z_string_len(z_view_string_loan(&ks)));

  // TODO(#3): Extract encoding from z_sample_encoding(sample).
  // zenoh-c 1.9.0 does not expose encoding-to-string conversion;
  // "sitos.v1" is the only encoding used in v0.1.
  Encoding enc;
  enc.id = "sitos.v1";

  if (!(*c->sink)(key_str, std::span<const std::byte>(data, len), enc)) {
    c->stop.store(true, std::memory_order_relaxed);
  }
  z_drop(z_move(slice));
}

}  // namespace

// ---------------------------------------------------------------------------
// TransportQuery
// ---------------------------------------------------------------------------

struct TransportQuery::Impl {
  const z_loaned_query_t* query = nullptr;
  std::shared_ptr<bool> query_alive_;
};

TransportQuery::TransportQuery() = default;
TransportQuery::~TransportQuery() = default;
TransportQuery::TransportQuery(TransportQuery&&) noexcept = default;
TransportQuery& TransportQuery::operator=(TransportQuery&&) noexcept = default;

Result<void> TransportQuery::Reply(std::string_view key, std::span<const std::byte> payload,
                           Encoding encoding) {
  if (!impl_ || !impl_->query || !*impl_->query_alive_) {
    return Result<void>::Err(MakeError(TransportErrc::kErrNoQuery));
  }

  auto enc = MakeEncoding(encoding);
  if (!enc.IsOk()) return Result<void>::Err(enc.Error());

  auto ke = MakeKeyexpr(key);
  if (!ke.IsOk()) return Result<void>::Err(ke.Error());

  auto p = MakeBytes(payload);
  if (!p.IsOk()) return Result<void>::Err(p.Error());

  z_query_reply_options_t opts;
  z_query_reply_options_default(&opts);
  opts.encoding = enc.Value().moved();

  z_result_t rc =
      z_query_reply(impl_->query, ke.Value().loan(), p.Value().moved(), &opts);

  if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
  return Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

struct Subscription::Impl {
  z_owned_subscriber_t subscriber;
};

Subscription::Subscription() = default;
Subscription::~Subscription() {
  if (impl_) z_drop(z_move(impl_->subscriber));
}
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&&) noexcept = default;

// ---------------------------------------------------------------------------
// Queryable
// ---------------------------------------------------------------------------

struct Queryable::Impl {
  z_owned_queryable_t queryable;
  std::function<void(TransportQuery&)> callback;
  // Shared with in-flight TransportQuery callbacks to detect
  // use-after-destruction. Set to false in ~Queryable() before dropping
  // the queryable, so any late Reply() call becomes a no-op.
  std::shared_ptr<bool> query_alive_{std::make_shared<bool>(true)};
};

Queryable::Queryable() = default;
Queryable::~Queryable() {
  if (impl_) {
    *impl_->query_alive_ = false;
    z_drop(z_move(impl_->queryable));
  }
}
Queryable::Queryable(Queryable&&) noexcept = default;
Queryable& Queryable::operator=(Queryable&&) noexcept = default;

// ---------------------------------------------------------------------------
// ZenohTransport
// ---------------------------------------------------------------------------

namespace {

bool OpenZenohSession(z_owned_session_t* session) {
  z_owned_config_t config;
  z_config_default(&config);
  return z_open(session, z_move(config), nullptr) == Z_OK;
}

}  // namespace

class ZenohTransport : public Transport {
 public:
  ZenohTransport()
      : session_valid_(OpenZenohSession(&session_)) {}

  bool IsSessionValid() const { return session_valid_; }

  ~ZenohTransport() override {
    if (session_valid_) {
      z_close(z_session_loan_mut(&session_), nullptr);
    }
    z_drop(z_move(session_));
  }

  Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                   Encoding encoding, PutOptions /*options*/) override {
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

    z_result_t rc =
        z_put(z_session_loan(&session_), ke.Value().loan(), p.Value().moved(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Delete(std::string_view key, PutOptions /*options*/) override {
    if (!session_valid_) return Result<void>::Err(MakeError(TransportErrc::kErrDisconnected));

    auto ke = MakeKeyexpr(key);
    if (!ke.IsOk()) return Result<void>::Err(ke.Error());

    z_delete_options_t opts;
    z_delete_options_default(&opts);

    z_result_t rc =
        z_delete(z_session_loan(&session_), ke.Value().loan(), &opts);

    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds timeout) override {
    if (!session_valid_) return Result<void>::Err(MakeError(TransportErrc::kErrDisconnected));
    if (timeout.count() < 0) return Result<void>::Err(MakeError(TransportErrc::kErrInvalidArg));

    auto ke = MakeKeyexpr(keyexpr);
    if (!ke.IsOk()) return Result<void>::Err(ke.Error());

    GetReplyCtx reply_ctx{&sink, false};

    z_owned_closure_reply_t closure;
    z_closure_reply(&closure, OnGetReply, nullptr, &reply_ctx);

    z_get_options_t opts;
    z_get_options_default(&opts);
    opts.timeout_ms = static_cast<uint64_t>(timeout.count());

    z_result_t rc =
        z_get(z_session_loan(&session_), ke.Value().loan(), "",
              z_move(closure), &opts);

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

  Queryable DeclareQueryable(
      std::string_view keyexpr_str,
      const std::function<void(TransportQuery&)>& callback) override {
    Queryable q;
    q.impl_ = std::make_unique<Queryable::Impl>();
    if (!session_valid_) {
      q.impl_.reset();
      return q;
    }

    q.impl_->callback = callback;

    auto ke = MakeKeyexpr(keyexpr_str);
    if (!ke.IsOk()) {
      q.impl_.reset();
      return q;
    }

    z_owned_closure_query_t closure;
    z_closure_query(
        &closure,
        +[](z_loaned_query_t* query, void* context) {
          auto* impl = static_cast<Queryable::Impl*>(context);
          TransportQuery tq;
          tq.impl_ = std::make_unique<TransportQuery::Impl>();
          tq.impl_->query = query;
          tq.impl_->query_alive_ = impl->query_alive_;

          z_view_string_t qks;
          z_keyexpr_as_view_string(z_query_keyexpr(query), &qks);
          tq.keyexpr = std::string(z_string_data(z_view_string_loan(&qks)),
                                   z_string_len(z_view_string_loan(&qks)));

          (impl->callback)(tq);
        },
        nullptr,  // impl is owned by Queryable::Impl, no cleanup
        q.impl_.get());

    z_queryable_options_t q_opts;
    z_queryable_options_default(&q_opts);

    z_result_t decl_rc = z_declare_queryable(
        z_session_loan(&session_), &q.impl_->queryable,
        ke.Value().loan(), z_move(closure), &q_opts);

    if (decl_rc != Z_OK) {
      q.impl_.reset();
    }
    return q;
  }

 private:
  z_owned_session_t session_;
  bool session_valid_ = false;
};

}  // namespace sitos

std::unique_ptr<sitos::Transport> sitos::MakeZenohTransport() {
  auto t = std::make_unique<sitos::ZenohTransport>();
  if (!t->IsSessionValid()) return nullptr;
  return t;
}
