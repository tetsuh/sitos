// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/client_config.hpp"

int main() {
  sitos::ClientConfig config;
  return sitos::ValidateClientConfig(config).IsOk() ? 0 : 1;
}
