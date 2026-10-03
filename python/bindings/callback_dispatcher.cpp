// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native side of Python subscription callback dispatch (Issue #26, docs/05 §3).

#include "callback_dispatcher.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>

#include <utility>

#include "param_value_conversion.hpp"

namespace nb = nanobind;

namespace sitos::python::detail {

void CallbackChannel::Push(const ParamChange& change) {
  {
    std::scoped_lock lock(mutex_);
    if (closed_) return;
    pending_.push_back(change);
  }
  changed_.notify_one();
}

std::optional<ParamChange> CallbackChannel::Pop() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [this] { return closed_ || !pending_.empty(); });
  if (closed_) return std::nullopt;
  ParamChange change = std::move(pending_.front());
  pending_.pop_front();
  return change;
}

void CallbackChannel::Close() noexcept {
  {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    pending_.clear();
  }
  changed_.notify_all();
}

std::size_t CallbackChannel::Pending() {
  std::scoped_lock lock(mutex_);
  return pending_.size();
}

ParamCallback MakeChannelCallback(std::shared_ptr<CallbackChannel> channel) {
  return [channel = std::move(channel)](const ParamChange& change) { channel->Push(change); };
}

PySubscriptionChannel::PySubscriptionChannel(std::shared_ptr<CallbackChannel> channel,
                                             ParamSubscription subscription)
    : channel_(std::move(channel)), subscription_(std::move(subscription)) {}

PySubscriptionChannel::~PySubscriptionChannel() {
  nb::gil_scoped_release release;
  CloseReleased();
}

nb::object PySubscriptionChannel::Next() {
  std::optional<ParamChange> change;
  {
    nb::gil_scoped_release release;
    change = channel_->Pop();
  }
  if (!change) return nb::none();
  const char* kind = change->kind == ParamChangeKind::kPut ? "put" : "delete";
  nb::object value = change->value ? ParamValueToPython(*change->value) : nb::none();
  return nb::make_tuple(kind, change->key, value);
}

void PySubscriptionChannel::Close() {
  nb::gil_scoped_release release;
  CloseReleased();
}

std::size_t PySubscriptionChannel::Pending() { return channel_->Pending(); }

void PySubscriptionChannel::CloseReleased() noexcept {
  // Native Close waits only for callbacks that push into the channel, never for Python.
  std::scoped_lock lock(close_mutex_);
  subscription_.Close();
  channel_->Close();
}

void BindCallbackDispatcher(nb::module_& python_module) {
  nb::class_<PySubscriptionChannel>(python_module, "_SubscriptionChannel")
      .def("next", &PySubscriptionChannel::Next)
      .def("close", &PySubscriptionChannel::Close)
      .def("_pending", &PySubscriptionChannel::Pending);
}

}  // namespace sitos::python::detail
