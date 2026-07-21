// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_SRC_TRANSPORT_ZENOH_RUNTIME_ANCHOR_HPP_
#define SITOS_SRC_TRANSPORT_ZENOH_RUNTIME_ANCHOR_HPP_

#include <cstdint>

namespace sitos::detail {

/// Returns an address in zenoh-c so binary consumers retain the required runtime dependency.
std::uintptr_t ZenohRuntimeAnchor() noexcept;

}  // namespace sitos::detail

#endif  // SITOS_SRC_TRANSPORT_ZENOH_RUNTIME_ANCHOR_HPP_
