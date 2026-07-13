// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>

namespace sitos::transport_internal {

// Invokes and clears a declaration reset handler without allowing exceptions
// to escape its noexcept caller.
void InvokeResetHandler(std::function<void()>& reset_handler) noexcept;

}  // namespace sitos::transport_internal
