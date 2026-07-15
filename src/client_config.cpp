// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/client_config.hpp"

#include "sitos/key.hpp"

namespace sitos {

Result<void> ValidateClientConfig(const ClientConfig& config) {
  if (!IsValidPrefix(config.prefix)) {
    return Result<void>::Err(Status::InvalidKey, "invalid client prefix");
  }
  if (config.query_timeout <= std::chrono::milliseconds::zero()) {
    return Result<void>::Err(Status::InvalidArgument, "query timeout must be positive");
  }
  if (config.zenoh_config_json.has_value() && config.zenoh_config_json->empty()) {
    return Result<void>::Err(Status::InvalidArgument, "zenoh configuration must not be empty");
  }
  return Result<void>::Ok();
}

}  // namespace sitos
