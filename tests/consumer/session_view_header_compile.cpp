// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/session_view.hpp"

template <typename T>
concept HasPut = requires(T& value) {
  value.Put("key", sitos::ParamValue(std::int64_t{1}));
};
template <typename T>
concept HasDelete = requires(T& value) { value.Delete("key"); };

static_assert(!HasPut<sitos::SessionView>);
static_assert(!HasDelete<sitos::SessionView>);

int main() { return 0; }
