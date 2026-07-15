// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/result.hpp"

int main() {
  auto result = sitos::Result<int>::Ok(1);
  return result.Value() == 1 ? 0 : 1;
}
