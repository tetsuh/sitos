// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Dependency-free SHA-256 (FIPS 180-4) used for process-local ADR-0028 operation
// fingerprints. Not a security boundary: digests never leave the process.

#ifndef SITOS_SHA256_HPP
#define SITOS_SHA256_HPP

#include <array>
#include <cstddef>
#include <span>

namespace sitos {

[[nodiscard]] std::array<std::byte, 32> Sha256(std::span<const std::byte> data);

}  // namespace sitos

#endif  // SITOS_SHA256_HPP
