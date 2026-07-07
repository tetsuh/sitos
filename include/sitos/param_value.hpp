// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Typed parameter value and payload v1 codec.
// See docs/03_wire_protocol.md §2 (payload v1) and docs/04_api_cpp.md §1.

#ifndef SITOS_PARAM_VALUE_HPP
#define SITOS_PARAM_VALUE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace sitos {

/// The five payload v1 types. Index order matches the wire type tag.
/// See docs/03_wire_protocol.md §2.1.
enum class ValueType : std::uint8_t {
  Bool = 0,
  S64 = 1,
  Dp = 2,
  Str = 3,
  Bytes = 4,
};

/// An immutable, typed parameter value. Thread-safe (no mutable state after
/// construction). See docs/04_api_cpp.md §1.
class ParamValue {
 public:
  using Variant = std::variant<bool, std::int64_t, double, std::string, std::vector<std::byte>>;

  /// Numeric inputs are normalized: integrals go to S64, floats to DP, bool to
  /// Bool. String literals are accepted as STR. std::vector<std::byte> as BYTES.
  template <typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, ParamValue>)
  explicit ParamValue(T&& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
      value_ = static_cast<bool>(v);
    } else if constexpr (std::is_integral_v<D>) {
      value_ = static_cast<std::int64_t>(v);
    } else if constexpr (std::is_floating_point_v<D>) {
      value_ = static_cast<double>(v);
    } else if constexpr (std::is_convertible_v<D, std::string_view>) {
      value_ = std::string(std::forward<T>(v));
    } else if constexpr (std::is_same_v<D, std::vector<std::byte>>) {
      value_ = std::forward<T>(v);
    } else if constexpr (std::is_same_v<D, std::byte>) {
      value_ = std::vector<std::byte>{std::forward<T>(v)};
    } else {
      static_assert(sizeof(D) == 0, "Unsupported ParamValue element type");
    }
  }

  ParamValue(const ParamValue&) = default;
  ParamValue(ParamValue&&) noexcept = default;
  ParamValue& operator=(const ParamValue&) = default;
  ParamValue& operator=(ParamValue&&) noexcept = default;

  /// Current value kind. Matches the wire type tag value.
  ValueType type() const noexcept { return static_cast<ValueType>(value_.index()); }

  /// Typed extraction. Arithmetic casts are allowed among Bool/S64/Dp; string
  /// and bytes require an exact type match. Returns std::nullopt on
  /// impossible conversions.
  template <typename T>
  std::optional<T> As() const {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
      if (auto* p = std::get_if<bool>(&value_)) return *p;
      if (auto* p = std::get_if<std::int64_t>(&value_)) return *p != 0;
      if (auto* p = std::get_if<double>(&value_)) return *p != 0.0;
      return std::nullopt;
    } else if constexpr (std::is_integral_v<D>) {
      if (auto* p = std::get_if<bool>(&value_)) return static_cast<T>(*p);
      if (auto* p = std::get_if<std::int64_t>(&value_)) return static_cast<T>(*p);
      if (auto* p = std::get_if<double>(&value_)) return static_cast<T>(*p);
      return std::nullopt;
    } else if constexpr (std::is_floating_point_v<D>) {
      if (auto* p = std::get_if<bool>(&value_)) return static_cast<T>(*p);
      if (auto* p = std::get_if<std::int64_t>(&value_)) return static_cast<T>(*p);
      if (auto* p = std::get_if<double>(&value_)) return static_cast<T>(*p);
      return std::nullopt;
    } else if constexpr (std::is_same_v<D, std::string>) {
      if (auto* p = std::get_if<std::string>(&value_)) return *p;
      return std::nullopt;
    } else if constexpr (std::is_same_v<D, std::vector<std::byte>>) {
      if (auto* p = std::get_if<std::vector<std::byte>>(&value_)) return *p;
      return std::nullopt;
    } else {
      return std::nullopt;
    }
  }

  /// View a Bytes value as an array of T (zero-copy). T must be trivially
  /// copyable. Returns std::nullopt if the value is not Bytes or its size is
  /// not a multiple of sizeof(T).
  template <typename T>
  std::optional<std::span<const T>> AsSpan() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AsSpan requires a trivially copyable element type");
    auto* p = std::get_if<std::vector<std::byte>>(&value_);
    if (p == nullptr) return std::nullopt;
    if (p->size() % sizeof(T) != 0) return std::nullopt;
    if (p->empty()) return std::span<const T>{};
    auto* data = reinterpret_cast<const T*>(p->data());
    return std::span<const T>{data, p->size() / sizeof(T)};
  }

  /// Encode to payload v1 (type tag + little-endian body).
  std::vector<std::byte> Encode() const;

  /// Decode a payload v1 byte sequence. Returns std::nullopt for empty, unknown,
  /// or truncated payloads. NaN payloads normalize to the canonical quiet NaN.
  static std::optional<ParamValue> Decode(std::span<const std::byte> payload);

 private:
  /// Body only (no type tag). Shared with the batch codec.
  std::vector<std::byte> EncodeBody() const;

  Variant value_;
};

}  // namespace sitos

#endif  // SITOS_PARAM_VALUE_HPP