// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Transport handle stubs used when zenoh support is disabled.

#include "sitos/transport.hpp"

#include "transport/declaration_handle_lifecycle.hpp"

#include <utility>

namespace sitos {

struct TransportQuery::Impl {};
struct Subscription::Impl {};
struct Queryable::Impl {};

}  // namespace sitos

#include "transport/declaration_handle_lifecycle_impl.hpp"

namespace sitos {

TransportQuery::TransportQuery() = default;
TransportQuery::TransportQuery(ReplyHandler handler)
    : test_reply_handler_(std::move(handler)) {}
TransportQuery::~TransportQuery() = default;

Result<void> TransportQuery::Reply(std::string_view key, std::span<const std::byte> payload,
                                   Encoding encoding) {
  if (test_reply_handler_) return test_reply_handler_(key, payload, encoding);
  return Result<void>::Err(std::make_error_code(std::errc::operation_not_supported));
}

void Subscription::Reset() noexcept {
  impl_.reset();
  transport_internal::InvokeResetHandler(reset_handler_);
}

void Queryable::Reset() noexcept {
  impl_.reset();
  transport_internal::InvokeResetHandler(reset_handler_);
}

std::unique_ptr<Transport> MakeZenohTransport() { return nullptr; }

}  // namespace sitos
