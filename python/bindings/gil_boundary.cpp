// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "gil_boundary.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace sitos::python::detail {
namespace {

#if SITOS_PYTHON_TEST_SUPPORT
struct BarrierState {
  std::mutex mutex;
  std::condition_variable condition;
  bool armed = false;
  bool entered = false;
  bool released = false;
  GilBoundary boundary = GilBoundary::Constructor;
};

BarrierState& State() {
  static BarrierState state;
  return state;
}

std::optional<GilBoundary> ParseBoundary(std::string_view name) {
  if (name == "constructor") return GilBoundary::Constructor;
  if (name == "create_session") return GilBoundary::CreateSession;
  if (name == "stop") return GilBoundary::Stop;
  return std::nullopt;
}
#endif

}  // namespace

void WaitAtGilBoundary(GilBoundary boundary) {
#if SITOS_PYTHON_TEST_SUPPORT
  auto& state = State();
  std::unique_lock lock(state.mutex);
  if (!state.armed || state.boundary != boundary) return;
  state.entered = true;
  state.condition.notify_all();
  state.condition.wait(lock, [&state] { return state.released || !state.armed; });
#else
  static_cast<void>(boundary);
#endif
}

void ArmGilBoundary(std::string_view boundary) {
#if SITOS_PYTHON_TEST_SUPPORT
  const auto parsed = ParseBoundary(boundary);
  if (!parsed.has_value()) throw std::invalid_argument("unknown GIL boundary");
  auto& state = State();
  std::lock_guard lock(state.mutex);
  if (state.armed) throw std::logic_error("GIL boundary is already armed");
  state.armed = true;
  state.entered = false;
  state.released = false;
  state.boundary = *parsed;
#else
  static_cast<void>(boundary);
  throw std::runtime_error("GIL test support is disabled");
#endif
}

bool WaitForGilBoundary(std::string_view boundary, std::chrono::milliseconds timeout) {
#if SITOS_PYTHON_TEST_SUPPORT
  const auto parsed = ParseBoundary(boundary);
  if (!parsed.has_value()) throw std::invalid_argument("unknown GIL boundary");
  auto& state = State();
  std::unique_lock lock(state.mutex);
  return state.condition.wait_for(lock, timeout, [&state, parsed] {
    return state.armed && state.boundary == *parsed && state.entered;
  });
#else
  static_cast<void>(boundary);
  static_cast<void>(timeout);
  return false;
#endif
}

void ReleaseGilBoundary(std::string_view boundary) {
#if SITOS_PYTHON_TEST_SUPPORT
  const auto parsed = ParseBoundary(boundary);
  if (!parsed.has_value()) throw std::invalid_argument("unknown GIL boundary");
  auto& state = State();
  std::lock_guard lock(state.mutex);
  if (!state.armed || state.boundary != *parsed) {
    throw std::logic_error("GIL boundary is not armed");
  }
  state.released = true;
  state.condition.notify_all();
#else
  static_cast<void>(boundary);
  throw std::runtime_error("GIL test support is disabled");
#endif
}

void ResetGilBoundary() {
#if SITOS_PYTHON_TEST_SUPPORT
  auto& state = State();
  std::lock_guard lock(state.mutex);
  state.armed = false;
  state.entered = false;
  state.released = true;
  state.condition.notify_all();
#endif
}

}  // namespace sitos::python::detail
