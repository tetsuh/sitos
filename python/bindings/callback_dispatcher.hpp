// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native side of Python subscription callback dispatch (Issue #26, docs/05 §3).

#ifndef SITOS_PYTHON_BINDINGS_CALLBACK_DISPATCHER_HPP_
#define SITOS_PYTHON_BINDINGS_CALLBACK_DISPATCHER_HPP_

#include <nanobind/nanobind.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "sitos/param_subscription.hpp"

namespace sitos::python::detail {

/// Unbounded FIFO between native subscription callbacks and one Python dispatcher thread
/// (DEC-26-003). Push never blocks on Python and never touches the GIL.
class CallbackChannel {
 public:
  void Push(const ParamChange& change);
  /// Blocks until a change is queued or the channel closes; the caller releases the GIL.
  std::optional<ParamChange> Pop();
  /// Discards pending changes and wakes the dispatcher; later pushes are dropped.
  void Close() noexcept;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<ParamChange> pending_;
  bool closed_ = false;
};

/// Owns a native ParamSubscription whose callback only feeds a CallbackChannel.
/// Python objects never cross into the native callback (DEC-26-007).
class PySubscriptionChannel {
 public:
  PySubscriptionChannel(std::shared_ptr<CallbackChannel> channel, ParamSubscription subscription);
  PySubscriptionChannel(const PySubscriptionChannel&) = delete;
  PySubscriptionChannel& operator=(const PySubscriptionChannel&) = delete;
  ~PySubscriptionChannel();

  /// Returns the next `(kind, key, value)` tuple, or None once closed.
  nanobind::object Next();
  /// Stops native delivery, waits for native callbacks, and discards pending changes.
  void Close();

 private:
  void CloseReleased() noexcept;

  std::shared_ptr<CallbackChannel> channel_;
  std::mutex close_mutex_;
  ParamSubscription subscription_;
};

/// Builds the native callback that feeds `channel`.
ParamCallback MakeChannelCallback(std::shared_ptr<CallbackChannel> channel);

void BindCallbackDispatcher(nanobind::module_& python_module);

}  // namespace sitos::python::detail

#endif  // SITOS_PYTHON_BINDINGS_CALLBACK_DISPATCHER_HPP_
