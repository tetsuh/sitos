// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/sitos.hpp"

int main() {
  sitos::ClientConfig config;
  auto validation = sitos::ValidateClientConfig(config);
  sitos::ParamValue value(1);
  static_cast<void>(sitos::ParamCache::Open(std::shared_ptr<sitos::Transport>{}));
  sitos::InMemoryEngine engine;
  static_cast<void>(engine);
  return validation.IsOk() && value.As<int>() == 1 ? 0 : 1;
}
