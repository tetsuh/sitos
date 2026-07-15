// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/batch.hpp"
#include "sitos/client_config.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/key.hpp"
#include "sitos/logging.hpp"
#include "sitos/param_value.hpp"
#include "sitos/result.hpp"
#include "sitos/session.hpp"
#include "sitos/sitos.hpp"
#include "sitos/status.hpp"
#include "sitos/storage_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

int main() {
  sitos::ClientConfig config;
  return sitos::ValidateClientConfig(config).IsOk() ? 0 : 1;
}
