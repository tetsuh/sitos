// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_TRANSPORT_CONFIG_FAILURE_HPP
#define SITOS_TRANSPORT_CONFIG_FAILURE_HPP

#include "sitos/status.hpp"

namespace sitos::transport_detail {

/// Classifies configuration creation failures without inspecting native text.
constexpr Status ConfigFailureStatus(bool user_config_provided) noexcept {
  return user_config_provided ? Status::InvalidArgument : Status::Error;
}

}  // namespace sitos::transport_detail

#endif  // SITOS_TRANSPORT_CONFIG_FAILURE_HPP
