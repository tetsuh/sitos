// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// In-process read-only composite view over a session overlay and snapshot.

#include "sitos/session_view.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "list_prefix_validation.hpp"
#include "sitos/key.hpp"
#include "sitos/storage_node.hpp"

namespace sitos {
namespace {

Result<void> InvalidArgument(std::string_view message) {
  return Result<void>::Err(Status::InvalidArgument, std::string(message));
}

Result<void> Disconnected() {
  return Result<void>::Err(Status::Disconnected, "StorageNode is stopped");
}

Result<void> NotFound() {
  return Result<void>::Err(Status::NotFound, "session or parameter not found");
}

Result<void> StorageError(std::string_view message) {
  return Result<void>::Err(Status::Error, std::string(message));
}

bool SameOwner(const std::weak_ptr<void>& expected,
               const std::shared_ptr<const StorageEngine>& actual) {
  if (expected.expired()) return false;
  return !expected.owner_before(actual) && !actual.owner_before(expected);
}

struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

struct ListItem {
  std::string key;
  ParamValue value;
};

}  // namespace

struct SessionView::Impl {
  std::weak_ptr<StorageNode::State> state;
  std::weak_ptr<void> overlay_owner;
  std::string sid;
};

struct SessionView::Readers {
  std::shared_ptr<void> state_owner;
  std::optional<StorageNode::State::CallbackLease> lease;
  std::optional<StorageNode::SessionRecord::AdmissionLease> admission;
  std::shared_ptr<StorageEngine> overlay;
  std::shared_ptr<const StorageReader> snapshot;
};

SessionView::SessionView(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

SessionView::~SessionView() = default;

SessionView::SessionView(SessionView&&) noexcept = default;

SessionView& SessionView::operator=(SessionView&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}

Result<SessionView> SessionView::Open(const StorageNode& node, std::string_view sid) {
  if (!IsValidSessionId(sid)) {
    return Result<SessionView>::Err(Status::InvalidKey, "invalid session id");
  }

  std::shared_ptr<StorageNode::State> state;
  {
    std::scoped_lock lock(node.lifecycle_mutex_);
    state = node.state_;
  }
  if (!state) {
    return Result<SessionView>::Err(Status::Disconnected, "StorageNode is stopped");
  }

  auto lease = state->Enter();
  if (!lease) return Result<SessionView>::Err(Status::Disconnected, "StorageNode is stopped");

  std::weak_ptr<void> overlay_owner;
  std::optional<StorageNode::SessionRecord::AdmissionLease> admission;
  {
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(sid);
    if (it == state->sessions.end() || !it->second->IsActive() ||
        !it->second->overlay) {
      return Result<SessionView>::Err(Status::NotFound, "session not found");
    }
    admission = it->second->TryAcquire();
    if (!admission.has_value()) {
      return Result<SessionView>::Err(Status::NotFound, "session not found");
    }
    const std::shared_ptr<StorageEngine>& overlay = it->second->overlay;
    overlay_owner = overlay;
  }

  auto impl = std::make_unique<Impl>();
  impl->state = state;
  impl->overlay_owner = std::move(overlay_owner);
  impl->sid = std::string(sid);
  return Result<SessionView>::Ok(SessionView(std::move(impl)));
}

Result<SessionView::Readers> SessionView::AcquireReaders() const {
  if (!impl_) return Result<Readers>::Err(Status::InvalidArgument, "moved-from SessionView");

  auto state = impl_->state.lock();
  if (!state) return Result<Readers>::ErrFrom(Disconnected());
  auto lease = state->Enter();
  if (!lease) return Result<Readers>::ErrFrom(Disconnected());

  Readers readers;
  readers.state_owner = state;
  readers.lease = std::move(lease);
  {
    std::shared_lock lock(state->session_mutex);
    auto it = state->sessions.find(impl_->sid);
    if (it == state->sessions.end() || !it->second->IsActive()) {
      return Result<Readers>::ErrFrom(NotFound());
    }
    const auto& record = it->second;
    readers.admission = record->TryAcquire();
    if (!readers.admission.has_value() || !record->snapshot || !record->overlay ||
        !SameOwner(impl_->overlay_owner, record->overlay)) {
      return Result<Readers>::ErrFrom(NotFound());
    }
    readers.overlay = record->overlay;
    readers.snapshot = record->snapshot;
  }
  return Result<Readers>::Ok(std::move(readers));
}

Result<ParamValue> SessionView::Read(const Readers& readers, std::string_view key) const {
  std::optional<ParamValue> value;
  bool found = false;
  try {
    found = readers.overlay->Get(key, [&value](std::string_view, Bytes bytes) {
      value = ParamValue::Decode(bytes);
      return true;
    });
  } catch (...) {
    return Result<ParamValue>::Err(Status::Error, "storage reader threw during Get");
  }
  if (found) {
    if (!value.has_value()) {
      return Result<ParamValue>::Err(Status::Error, "malformed parameter payload");
    }
    return Result<ParamValue>::Ok(std::move(*value));
  }

  value.reset();
  try {
    found = readers.snapshot->Get(key, [&value](std::string_view, Bytes bytes) {
      value = ParamValue::Decode(bytes);
      return true;
    });
  } catch (...) {
    return Result<ParamValue>::Err(Status::Error, "storage reader threw during Get");
  }
  if (!found) return Result<ParamValue>::ErrFrom(NotFound());
  if (!value.has_value()) {
    return Result<ParamValue>::Err(Status::Error, "malformed parameter payload");
  }
  return Result<ParamValue>::Ok(std::move(*value));
}

Result<ParamValue> SessionView::Get(std::string_view key) const {
  if (!impl_) return Result<ParamValue>::Err(Status::InvalidArgument, "moved-from SessionView");
  if (!IsValidKey(key)) return Result<ParamValue>::Err(Status::InvalidKey, "invalid key");
  auto acquired = AcquireReaders();
  if (!acquired.IsOk()) return Result<ParamValue>::ErrFrom(acquired);
  return Read(acquired.Value(), key);
}

Result<bool> SessionView::Contains(std::string_view key) const {
  if (!impl_) return Result<bool>::Err(Status::InvalidArgument, "moved-from SessionView");
  if (!IsValidKey(key)) return Result<bool>::Err(Status::InvalidKey, "invalid key");
  auto acquired = AcquireReaders();
  if (!acquired.IsOk()) return Result<bool>::ErrFrom(acquired);
  auto value = Read(acquired.Value(), key);
  if (value.IsOk()) return Result<bool>::Ok(true);
  if (value.StatusCode() == Status::NotFound) return Result<bool>::Ok(false);
  return Result<bool>::ErrFrom(value);
}

Result<void> SessionView::List(std::string_view prefix, const ListSink& sink) const {
  if (!impl_) return InvalidArgument("moved-from SessionView");
  auto prefix_result = param_detail::ValidateListPrefix(prefix);
  if (!prefix_result.IsOk()) return prefix_result;
  if (!sink) return InvalidArgument("null List sink");
  auto acquired = AcquireReaders();
  if (!acquired.IsOk()) return Result<void>::ErrFrom(acquired);
  auto readers = std::move(acquired).Value();

  std::vector<ListItem> values;
  std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> overlay_keys;
  std::optional<Result<void>> decode_error;
  try {
    const bool overlay_completed = readers.overlay->List(prefix, [&decode_error, &overlay_keys, &values](
        std::string_view key, Bytes bytes) {
      auto decoded = ParamValue::Decode(bytes);
      if (!decoded.has_value()) {
        decode_error = StorageError("malformed parameter payload");
        return false;
      }
      overlay_keys.emplace(key);
      values.emplace_back(std::string(key), std::move(*decoded));
      return true;
    });
    if (!overlay_completed && !decode_error.has_value()) {
      return StorageError("storage reader stopped List unexpectedly");
    }
    if (decode_error.has_value()) return *decode_error;

    const bool snapshot_completed = readers.snapshot->List(prefix, [&decode_error, &overlay_keys, &values](
        std::string_view key, Bytes bytes) {
      if (overlay_keys.contains(key)) return true;
      auto decoded = ParamValue::Decode(bytes);
      if (!decoded.has_value()) {
        decode_error = StorageError("malformed parameter payload");
        return false;
      }
      values.emplace_back(std::string(key), std::move(*decoded));
      return true;
    });
    if (!snapshot_completed && !decode_error.has_value()) {
      return StorageError("storage reader stopped List unexpectedly");
    }
  } catch (...) {
    return StorageError("storage reader threw during List");
  }
  if (decode_error.has_value()) return *decode_error;

  std::ranges::sort(values, [](const ListItem& left, const ListItem& right) {
    return left.key < right.key;
  });

  readers.overlay.reset();
  readers.snapshot.reset();
  readers.admission.reset();
  readers.lease.reset();
  readers.state_owner.reset();
  for (const auto& item : values) {
    if (!sink(item.key, item.value)) break;
  }
  return Result<void>::Ok();
}

}  // namespace sitos
