// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// StorageNode-internal ADR-0028 token state machine: one node-wide registry of
// Processing and Completed acknowledgement tokens with process-local operation
// fingerprints, per-lane single admission, and a bounded completion-order ring.
// Internal to StorageNode; not an installed header.

#ifndef SITOS_ACK_REGISTRY_HPP
#define SITOS_ACK_REGISTRY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "sitos/ack.hpp"
#include "sitos/transport.hpp"

namespace sitos {

/// SHA-256 over operation kind, full key, normalized Encoding identifier, and
/// payload bytes. Process-local collision detection only; never sent on the wire.
struct AckFingerprint {
  std::array<std::byte, 32> bytes{};

  bool operator==(const AckFingerprint&) const = default;
};

[[nodiscard]] AckFingerprint ComputeAckFingerprint(AckOperationKind kind, std::string_view full_key,
                                                   std::string_view encoding_id,
                                                   std::span<const std::byte> payload);

class AckRegistry {
 public:
  static constexpr std::size_t kDefaultCapacity = 4096;
  /// The serialized parameter-write lane (base/session Put and PutBatch).
  static constexpr std::uint64_t kParameterLane = 0;

  enum class ClaimOutcome {
    Admitted,             ///< token is now Processing in the lane; caller must Complete it
    DuplicateProcessing,  ///< same token and fingerprint still Processing: no result yet
    DuplicateCompleted,   ///< same token and fingerprint already Completed: reuse the result
    Collision,            ///< same token, different fingerprint: reject, keep the original
    LaneBusy,             ///< another token is Processing in this lane: reentrant admission
  };

  explicit AckRegistry(std::size_t capacity = kDefaultCapacity);

  /// Atomically claims the token before any mutation. Never blocks on callers.
  [[nodiscard]] ClaimOutcome Claim(const AckToken& token, const AckFingerprint& fingerprint,
                                   std::uint64_t lane);

  /// Claims a token or atomically retains `lane_busy_result` when another token owns the lane.
  /// A LaneBusy token can therefore never become admissible after the current owner completes.
  [[nodiscard]] ClaimOutcome ClaimOrReject(const AckToken& token, const AckFingerprint& fingerprint,
                                           std::uint64_t lane, AckResultV1 lane_busy_result);

  /// Publishes the immutable result for an Admitted token and frees its lane.
  /// Returns false when the token is not Processing (already completed or unknown).
  bool Complete(const AckToken& token, AckResultV1 result);

  /// Retains a Completed result for a token that was never admitted (for
  /// example a LaneBusy rejection). Returns false when the token is already retained.
  bool RecordRejected(const AckToken& token, const AckFingerprint& fingerprint, AckResultV1 result);

  /// The Completed result, or nullopt for absent, Processing, or evicted tokens.
  [[nodiscard]] std::optional<AckResultV1> Find(const AckToken& token) const;

  /// Drops every Processing and Completed entry (StorageNode Stop).
  void Clear();

  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] std::size_t ProcessingCount() const;

 private:
  struct Entry {
    AckFingerprint fingerprint;
    std::uint64_t lane = 0;
    std::optional<AckResultV1> result;  // nullopt while Processing
  };
  struct TokenHash {
    std::size_t operator()(const AckToken& token) const noexcept;
  };

  // Must be called with mutex_ held.
  ClaimOutcome ClaimLocked(const AckToken& token, const AckFingerprint& fingerprint,
                           std::uint64_t lane, std::optional<AckResultV1> lane_busy_result);

  // Must be called with mutex_ held; pushes the token onto the ring and evicts the oldest.
  void RetainCompletedLocked(const AckToken& token);

  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::unordered_map<AckToken, Entry, TokenHash> entries_;
  std::unordered_map<std::uint64_t, AckToken> processing_by_lane_;
  std::deque<AckToken> completion_order_;
};

}  // namespace sitos

#endif  // SITOS_ACK_REGISTRY_HPP
