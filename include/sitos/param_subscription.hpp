// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Delta subscription API for ParamStore.

#ifndef SITOS_PARAM_SUBSCRIPTION_HPP
#define SITOS_PARAM_SUBSCRIPTION_HPP

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "sitos/param_value.hpp"

namespace sitos {

/// Kind of change delivered by a ParamStore subscription.
enum class ParamChangeKind { kPut, kDelete };

/// Owned relative-key change delivered to a subscription callback.
struct ParamChange {
  ParamChangeKind kind;
  std::string key;
  std::optional<ParamValue> value;
};

/// Callback invoked for each matching change.
using ParamCallback = std::function<void(const ParamChange&)>;

/// Move-only, callback-quiescent subscription handle.
class ParamSubscription {
 public:
  ParamSubscription(const ParamSubscription&) = delete;
  ParamSubscription& operator=(const ParamSubscription&) = delete;
  ParamSubscription(ParamSubscription&& other) noexcept;
  ParamSubscription& operator=(ParamSubscription&& other) noexcept;
  ~ParamSubscription();

  /// Stops delivery and waits for all admitted work to finish.
  ///
  /// Calling Close, destroying this handle, or move-assigning this handle from its own callback
  /// is forbidden.
  void Close() noexcept;

 private:
  friend class ParamStore;
  struct Impl;
  explicit ParamSubscription(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_PARAM_SUBSCRIPTION_HPP
