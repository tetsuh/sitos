// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PARAM_CONCEPTS_HPP
#define SITOS_PARAM_CONCEPTS_HPP

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sitos/param_value.hpp"

namespace sitos {

template <typename T>
concept ParamStringInput =
    std::same_as<std::remove_cvref_t<T>, std::string> ||
    std::same_as<std::remove_cvref_t<T>, std::string_view> ||
    (std::is_pointer_v<std::decay_t<T>> &&
     std::same_as<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char>);

template <typename T>
concept ParamInput =
    !std::same_as<std::remove_cvref_t<T>, ParamValue> &&
    (std::same_as<std::remove_cvref_t<T>, bool> || std::integral<std::remove_cvref_t<T>> ||
     std::floating_point<std::remove_cvref_t<T>> ||
     std::same_as<std::remove_cvref_t<T>, std::byte> ||
     std::same_as<std::remove_cvref_t<T>, std::vector<std::byte>> || ParamStringInput<T>);

template <typename T>
concept SupportedParamType =
    std::same_as<std::remove_cvref_t<T>, bool> || std::integral<std::remove_cvref_t<T>> ||
    std::floating_point<std::remove_cvref_t<T>> ||
    std::same_as<std::remove_cvref_t<T>, std::string> ||
    std::same_as<std::remove_cvref_t<T>, std::vector<std::byte>>;

template <typename T>
concept ParamSpanElement = std::is_object_v<std::remove_cv_t<T>> &&
                          std::is_trivially_copyable_v<std::remove_cv_t<T>>;

}  // namespace sitos

#endif  // SITOS_PARAM_CONCEPTS_HPP
