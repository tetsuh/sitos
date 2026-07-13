// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "declaration_handle_lifecycle.hpp"

#include <utility>

namespace sitos::transport_internal {

void InvokeResetHandler(std::function<void()>& reset_handler) noexcept {
  if (!reset_handler) return;

  auto handler = std::move(reset_handler);
  reset_handler = nullptr;
  try {
    handler();
  } catch (...) {
    // Declaration reset is noexcept; reset handlers are best-effort cleanup hooks.
  }
}

}  // namespace sitos::transport_internal
