// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "query_error_reply.hpp"

#include <cassert>
#include <string_view>

namespace sitos::transport_internal {
namespace {

constexpr std::string_view kStateLostPayload = R"({"v":1,"status":10})";
constexpr std::string_view kCatalogUnavailablePayload = R"({"v":1,"status":11})";

bool Equals(std::span<const std::byte> payload, std::string_view expected) noexcept {
  if (payload.size() != expected.size()) return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (payload[i] != static_cast<std::byte>(expected[i])) return false;
  }
  return true;
}

}  // namespace

bool IsQueryErrorStatus(Status status) noexcept {
  return status == Status::StateLost || status == Status::CatalogUnavailable;
}

std::string EncodeQueryErrorReply(Status status) {
  assert(IsQueryErrorStatus(status));
  return std::string(status == Status::StateLost ? kStateLostPayload : kCatalogUnavailablePayload);
}

std::optional<Status> DecodeQueryErrorReply(std::span<const std::byte> payload) noexcept {
  // The two canonical byte strings are the whole v1 grammar, so an exact match
  // is the complete decoder: whitespace, key order, and extra fields are invalid.
  if (Equals(payload, kStateLostPayload)) return Status::StateLost;
  if (Equals(payload, kCatalogUnavailablePayload)) return Status::CatalogUnavailable;
  return std::nullopt;
}

}  // namespace sitos::transport_internal
