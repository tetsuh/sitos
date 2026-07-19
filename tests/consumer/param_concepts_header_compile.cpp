// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include "sitos/param_concepts.hpp"

static_assert(sitos::ParamInput<std::int64_t>);
static_assert(sitos::SupportedParamType<std::string>);
static_assert(sitos::ParamSpanElement<std::uint32_t>);

int main() { return 0; }
