// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "ack_registry.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "sha256.hpp"

namespace sitos {
namespace {

void AppendLengthPrefixed(std::vector<std::byte>& out, std::span<const std::byte> field) {
  const auto length = static_cast<std::uint64_t>(field.size());
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xFF));
  out.insert(out.end(), field.begin(), field.end());
}

std::span<const std::byte> AsBytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

AckFingerprint ComputeAckFingerprint(AckOperationKind kind, std::string_view full_key,
                                     std::string_view encoding_id,
                                     std::span<const std::byte> payload) {
  std::vector<std::byte> material;
  material.reserve(1 + 3 * 8 + full_key.size() + encoding_id.size() + payload.size());
  material.push_back(static_cast<std::byte>(kind));
  AppendLengthPrefixed(material, AsBytes(full_key));
  AppendLengthPrefixed(material, AsBytes(encoding_id));
  AppendLengthPrefixed(material, payload);
  return AckFingerprint{Sha256(material)};
}

std::size_t AckRegistry::TokenHash::operator()(const AckToken& token) const noexcept {
  // Tokens are uniformly random UUIDv4 bytes; the first 8 bytes are an adequate hash.
  std::uint64_t value = 0;
  std::memcpy(&value, token.bytes.data(), sizeof(value));
  return static_cast<std::size_t>(value);
}

AckRegistry::AckRegistry(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

AckRegistry::ClaimOutcome AckRegistry::Claim(const AckToken& token,
                                             const AckFingerprint& fingerprint,
                                             std::uint64_t lane) {
  std::scoped_lock lock(mutex_);
  if (auto it = entries_.find(token); it != entries_.end()) {
    if (it->second.fingerprint != fingerprint) return ClaimOutcome::Collision;
    return it->second.result.has_value() ? ClaimOutcome::DuplicateCompleted
                                         : ClaimOutcome::DuplicateProcessing;
  }
  if (processing_by_lane_.contains(lane)) return ClaimOutcome::LaneBusy;
  entries_.emplace(token, Entry{fingerprint, lane, std::nullopt});
  processing_by_lane_.emplace(lane, token);
  return ClaimOutcome::Admitted;
}

bool AckRegistry::Complete(const AckToken& token, AckResultV1 result) {
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(token);
  if (it == entries_.end() || it->second.result.has_value()) return false;
  it->second.result = std::move(result);
  if (auto lane = processing_by_lane_.find(it->second.lane);
      lane != processing_by_lane_.end() && lane->second == token) {
    processing_by_lane_.erase(lane);
  }
  RetainCompletedLocked(token);
  return true;
}

bool AckRegistry::RecordRejected(const AckToken& token, const AckFingerprint& fingerprint,
                                 AckResultV1 result) {
  std::scoped_lock lock(mutex_);
  if (entries_.contains(token)) return false;
  entries_.emplace(token, Entry{fingerprint, 0, std::move(result)});
  RetainCompletedLocked(token);
  return true;
}

void AckRegistry::RetainCompletedLocked(const AckToken& token) {
  completion_order_.push_back(token);
  while (completion_order_.size() > capacity_) {
    const AckToken oldest = completion_order_.front();
    completion_order_.pop_front();
    entries_.erase(oldest);  // only Completed tokens are ever in the ring
  }
}

std::optional<AckResultV1> AckRegistry::Find(const AckToken& token) const {
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(token);
  if (it == entries_.end()) return std::nullopt;
  return it->second.result;
}

void AckRegistry::Clear() {
  std::scoped_lock lock(mutex_);
  entries_.clear();
  processing_by_lane_.clear();
  completion_order_.clear();
}

std::size_t AckRegistry::Size() const {
  std::scoped_lock lock(mutex_);
  return entries_.size();
}

std::size_t AckRegistry::ProcessingCount() const {
  std::scoped_lock lock(mutex_);
  return processing_by_lane_.size();
}

}  // namespace sitos
