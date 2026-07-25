// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/param_subscription.hpp"

#include <type_traits>

int main() {
  static_assert(!std::is_default_constructible_v<sitos::ParamSubscription>);
  static_assert(!std::is_copy_constructible_v<sitos::ParamSubscription>);
  static_assert(std::is_move_constructible_v<sitos::ParamSubscription>);
  return 0;
}
