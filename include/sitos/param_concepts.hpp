// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PARAM_CONCEPTS_HPP
#define SITOS_PARAM_CONCEPTS_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sitos/param_value.hpp"
#include "sitos/result.hpp"

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

namespace param_detail {

template <ParamInput T>
Result<ParamValue> MakeParamValue(T&& value) {
  using D = std::decay_t<T>;
  if constexpr (std::is_pointer_v<D> &&
                std::is_same_v<std::remove_cv_t<std::remove_pointer_t<D>>, char>) {
    if (value == nullptr) {
      return Result<ParamValue>::Err(Status::InvalidArgument, "null string argument");
    }
  } else if constexpr (std::is_integral_v<D> && !std::is_same_v<D, bool>) {
    if (!std::in_range<std::int64_t>(value)) {
      return Result<ParamValue>::Err(Status::InvalidArgument,
                                     "integral value is outside payload range");
    }
  }
  return Result<ParamValue>::Ok(ParamValue(std::forward<T>(value)));
}

}  // namespace param_detail

}  // namespace sitos

#endif  // SITOS_PARAM_CONCEPTS_HPP
