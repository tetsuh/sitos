// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_LIST_SINK_HPP
#define SITOS_LIST_SINK_HPP

#include <functional>
#include <string_view>

#include "sitos/param_value.hpp"

namespace sitos {

using ListSink = std::function<bool(std::string_view key, const ParamValue& value)>;

}  // namespace sitos

#endif  // SITOS_LIST_SINK_HPP
