// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/status.hpp"

int main() {
  const auto error = sitos::MakeErrorCode(sitos::Status::Error);
  return error ? 0 : 1;
}
