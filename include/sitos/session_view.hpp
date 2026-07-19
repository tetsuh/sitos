// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// In-process read-only composite view over a session overlay and snapshot.

#ifndef SITOS_SESSION_VIEW_HPP
#define SITOS_SESSION_VIEW_HPP

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
#include "sitos/param_value.hpp"
#include "sitos/result.hpp"

namespace sitos {

class StorageNode;

/// Read-only host-process view resolving an overlay over a session snapshot.
class SessionView {
 public:
  static Result<SessionView> Open(const StorageNode& node, std::string_view sid);

  ~SessionView();
  SessionView(const SessionView&) = delete;
  SessionView& operator=(const SessionView&) = delete;
  SessionView(SessionView&&) noexcept;
  SessionView& operator=(SessionView&&) noexcept;

  Result<ParamValue> Get(std::string_view key) const;

  template <SupportedParamType T>
  Result<T> Get(std::string_view key) const {
    auto value = Get(key);
    if (!value.IsOk()) return Result<T>::ErrFrom(value);
    auto converted = value.Value().template As<T>();
    if (!converted.has_value()) {
      return Result<T>::Err(Status::TypeMismatch, "parameter value type mismatch");
    }
    return Result<T>::Ok(std::move(*converted));
  }

  template <SupportedParamType T>
  Result<T> GetOr(std::string_view key, T default_value) const {
    auto result = Get<T>(key);
    if (result.IsOk() || result.StatusCode() != Status::NotFound) return result;
    return Result<T>::Ok(std::move(default_value));
  }

  Result<bool> Contains(std::string_view key) const;
  Result<void> List(std::string_view prefix, const ListSink& sink) const;

 private:
  struct Impl;
  explicit SessionView(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_SESSION_VIEW_HPP
