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

#include <cstddef>
#include <string>

namespace sitos {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

std::error_code MakeError(z_result_t rc) {
  if (rc == Z_OK) return {};
  return {static_cast<int>(rc), std::generic_category()};
}

}  // namespace

// ---------------------------------------------------------------------------
// TransportQuery
// ---------------------------------------------------------------------------

struct TransportQuery::Impl {
  const z_loaned_query_t* query = nullptr;
};

TransportQuery::TransportQuery() = default;
TransportQuery::~TransportQuery() = default;
TransportQuery::TransportQuery(TransportQuery&&) noexcept = default;
TransportQuery& TransportQuery::operator=(TransportQuery&&) noexcept = default;

void TransportQuery::Reply(std::string_view key, std::span<const std::byte> payload,
                           Encoding encoding) {
  if (!impl_ || !impl_->query) return;

  z_owned_keyexpr_t ke;
  z_keyexpr_from_str(&ke, std::string(key).c_str());

  z_owned_bytes_t p;
  z_bytes_copy_from_buf(&p, reinterpret_cast<const uint8_t*>(payload.data()),
                        payload.size());

  z_query_reply_options_t opts;
  z_query_reply_options_default(&opts);

  z_owned_encoding_t z_enc;
  if (z_encoding_from_str(&z_enc, encoding.id.c_str()) == Z_OK) {
    opts.encoding = z_move(z_enc);
  }

  z_query_reply(impl_->query, z_keyexpr_loan(&ke), z_move(p), &opts);
  z_drop(z_move(ke));
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

struct Subscription::Impl {
  z_owned_subscriber_t subscriber;
};

Subscription::Subscription() = default;
Subscription::~Subscription() = default;
Subscription::Subscription(Subscription&&) noexcept = default;
Subscription& Subscription::operator=(Subscription&&) noexcept = default;

// ---------------------------------------------------------------------------
// Queryable
// ---------------------------------------------------------------------------

struct Queryable::Impl {
  z_owned_queryable_t queryable;
};

Queryable::Queryable() = default;
Queryable::~Queryable() = default;
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
    z_owned_keyexpr_t ke;
    z_keyexpr_from_str(&ke, std::string(key).c_str());

    z_owned_bytes_t p;
    z_bytes_copy_from_buf(&p, reinterpret_cast<const uint8_t*>(payload.data()),
                          payload.size());

    if (!session_valid_) return Result<void>::Err(MakeError(-1));

    z_put_options_t opts;
    z_put_options_default(&opts);

    z_owned_encoding_t z_enc;
    if (z_encoding_from_str(&z_enc, encoding.id.c_str()) == Z_OK) {
      opts.encoding = z_move(z_enc);
    }

    z_result_t rc =
        z_put(z_session_loan(&session_), z_keyexpr_loan(&ke), z_move(p), &opts);

    z_drop(z_move(ke));
    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Delete(std::string_view key, PutOptions /*options*/) override {
    z_owned_keyexpr_t ke;
    z_keyexpr_from_str(&ke, std::string(key).c_str());

    if (!session_valid_) return Result<void>::Err(MakeError(-1));

    z_delete_options_t opts;
    z_delete_options_default(&opts);

    z_result_t rc =
        z_delete(z_session_loan(&session_), z_keyexpr_loan(&ke), &opts);

    z_drop(z_move(ke));
    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                   std::chrono::milliseconds timeout) override {
    z_owned_keyexpr_t ke;
    z_keyexpr_from_str(&ke, std::string(keyexpr).c_str());

    struct Ctx {
      const QueryResultSink* sink;
      bool stop = false;
    } ctx{&sink, false};

    z_owned_closure_reply_t closure;
    z_closure_reply(
        &closure,
        +[](z_loaned_reply_t* reply, void* context) {
          auto* c = static_cast<Ctx*>(context);
          if (c->stop) return;

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
            c->stop = true;
          }
          z_drop(z_move(slice));
        },
        nullptr,  // drop — Ctx is stack-allocated, no cleanup needed
        &ctx);

    if (!session_valid_) return Result<void>::Err(MakeError(-1));

    z_get_options_t opts;
    z_get_options_default(&opts);
    opts.timeout_ms = static_cast<uint64_t>(timeout.count());

    z_result_t rc =
        z_get(z_session_loan(&session_), z_keyexpr_loan(&ke), "",
              z_move(closure), &opts);

    z_drop(z_move(ke));
    if (rc != Z_OK) return Result<void>::Err(MakeError(rc));
    return Result<void>::Ok();
  }

  Subscription DeclareSubscriber(
      std::string_view /*keyexpr*/,
      std::function<void(const TransportSample&)> /*callback*/) override {
    // TODO(#3): implement subscriber
    return {};
  }

  Queryable DeclareQueryable(
      std::string_view keyexpr_str,
      std::function<void(TransportQuery&)> callback) override {
    Queryable q;
    q.impl_ = std::make_unique<Queryable::Impl>();
    if (!session_valid_) return q;

    z_owned_keyexpr_t ke;
    z_keyexpr_from_str(&ke, std::string(keyexpr_str).c_str());

    auto* cb = new std::function<void(TransportQuery&)>(std::move(callback));

    z_owned_closure_query_t closure;
    z_closure_query(
        &closure,
        +[](z_loaned_query_t* query, void* context) {
          auto* f = static_cast<std::function<void(TransportQuery&)>*>(context);
          TransportQuery tq;
          tq.impl_ = std::make_unique<TransportQuery::Impl>();
          tq.impl_->query = query;
          (*f)(tq);
        },
        +[](void* context) {
          delete static_cast<std::function<void(TransportQuery&)>*>(context);
        },
        cb);

    z_queryable_options_t q_opts;
    z_queryable_options_default(&q_opts);

    z_declare_queryable(z_session_loan(&session_), &q.impl_->queryable,
                        z_keyexpr_loan(&ke), z_move(closure), &q_opts);

    z_drop(z_move(ke));
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
