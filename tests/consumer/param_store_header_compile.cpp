// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <type_traits>

#include "sitos/param_store.hpp"

static_assert(!std::is_copy_constructible_v<sitos::ParamStore>);
static_assert(std::is_move_constructible_v<sitos::ParamStore>);

int main() {
  auto result = sitos::ParamStore::Open(std::shared_ptr<sitos::Transport>{});
  return result.StatusCode() == sitos::Status::InvalidArgument ? 0 : 1;
}
