// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_cache.hpp"

template <typename T>
concept HasAttachBase = requires(T& value) {
  value.AttachBase();
};

static_assert(!HasAttachBase<sitos::ParamCache>);

int main() {
  sitos::ClientConfig config;
  auto result = sitos::ParamCache::Open(std::shared_ptr<sitos::Transport>{}, config);
  return result.IsOk() ? 1 : 0;
}
