// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/list_sink.hpp"

int main() {
  sitos::ListSink sink = [](std::string_view, const sitos::ParamValue&) { return true; };
  return sink ? 0 : 1;
}
