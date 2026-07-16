// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_CLIENT_CONFIG_HPP
#define SITOS_CLIENT_CONFIG_HPP

#include <chrono>
#include <optional>
#include <string>

#include "sitos/result.hpp"

namespace sitos {

struct ClientConfig {
  std::string prefix = "sitos";
  std::optional<std::string> zenoh_config_json;
  std::chrono::milliseconds query_timeout{5000};
};

Result<void> ValidateClientConfig(const ClientConfig& config);

}  // namespace sitos

#endif  // SITOS_CLIENT_CONFIG_HPP
