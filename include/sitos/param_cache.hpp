// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PARAM_CACHE_HPP
#define SITOS_PARAM_CACHE_HPP

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/client_config.hpp"
#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos {

namespace param_cache_test_access {
class ParamCacheTestAccess;
}
namespace fence_test_access {
class FenceTestAccess;
}

namespace param_cache_detail {
struct Access;
}

template <ParamSpanElement T>
struct SpanHandle {
  std::span<const T> span;
  std::shared_ptr<const ParamValue> keepalive;
};

/// Subscriber-side session cache with local reads and session-overlay writes.
class ParamCache {
 public:
  static Result<ParamCache> Open(ClientConfig config = {});
  static Result<ParamCache> Open(std::shared_ptr<Transport> transport, ClientConfig config = {});

  ~ParamCache();
  ParamCache(const ParamCache&) = delete;
  ParamCache& operator=(const ParamCache&) = delete;
  ParamCache(ParamCache&&) noexcept;
  ParamCache& operator=(ParamCache&&) noexcept;

  Result<void> Attach(std::string_view sid);
  void Detach() noexcept;

  Result<std::shared_ptr<const ParamValue>> GetShared(std::string_view key) const;

  template <SupportedParamType T>
  Result<T> Get(std::string_view key) const {
    auto shared = GetShared(key);
    if (!shared.IsOk()) return Result<T>::ErrFrom(shared);
    return param_detail::ConvertParamValue<T>(*shared.Value());
  }

  template <SupportedParamType T>
  Result<T> GetOr(std::string_view key, T default_value) const {
    auto result = Get<T>(key);
    if (result.IsOk() || result.StatusCode() != Status::NotFound) return result;
    return Result<T>::Ok(std::move(default_value));
  }

  template <ParamSpanElement T>
  Result<SpanHandle<T>> GetSpan(std::string_view key) const {
    auto shared = GetShared(key);
    if (!shared.IsOk()) return Result<SpanHandle<T>>::ErrFrom(shared);
    auto span = shared.Value()->template AsSpan<T>();
    if (!span.has_value()) {
      return Result<SpanHandle<T>>::Err(Status::TypeMismatch,
                                        "parameter value is not a compatible byte span");
    }
    auto keepalive = std::move(shared).Value();
    return Result<SpanHandle<T>>::Ok(SpanHandle<T>{*span, std::move(keepalive)});
  }

  Result<bool> Contains(std::string_view key) const;
  Result<void> List(std::string_view prefix, const ListSink& sink) const;

  Result<void> Put(std::string_view key, const ParamValue& value);

  template <ParamInput T>
  Result<void> Put(std::string_view key, T&& value) {
    auto converted = param_detail::MakeParamValue(std::forward<T>(value));
    if (!converted.IsOk()) return Result<void>::ErrFrom(converted);
    return Put(key, converted.Value());
  }

  Result<void> PutBatch(std::span<const BatchEntry> entries);

  /// Waits until this cache's own subscriber has processed every write admitted
  /// to its Transport lane before the Fence linearization point (ADR-0029).
  ///
  /// `timeout` is the total deadline and must be strictly positive. Success means
  /// the initiating cache's matching Attach generation completed the covered
  /// contiguous prefix; it does not prove peer-cache delivery, StorageNode
  /// acknowledgement, or durability. Writes admitted after that point are excluded.
  /// At most one wait may be pending per attached cache: a second concurrent call
  /// returns Status::InvalidArgument with std::errc::operation_in_progress and
  /// emits no marker. No valid completion before the deadline is Status::Timeout;
  /// the data and the marker are never resubmitted.
  Result<void> WaitForLocalDelivery(std::chrono::milliseconds timeout);

 private:
  struct Impl;
  friend struct param_cache_detail::Access;
  friend class param_cache_test_access::ParamCacheTestAccess;
  friend class fence_test_access::FenceTestAccess;
  explicit ParamCache(std::shared_ptr<Transport> transport, ClientConfig config);

  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_PARAM_CACHE_HPP
