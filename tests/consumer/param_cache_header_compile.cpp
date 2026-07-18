// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

int main() {
  sitos::ClientConfig config;
  auto result = sitos::ParamCache::Open(std::shared_ptr<sitos::Transport>{}, config);
  return result.IsOk() ? 1 : 0;
}
