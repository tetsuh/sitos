// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Thread-safe synchronous client operations against a StorageNode.

#ifndef SITOS_PARAM_STORE_HPP
#define SITOS_PARAM_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/client_config.hpp"
#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
#include "sitos/key.hpp"
#include "sitos/param_value.hpp"
#include "sitos/param_subscription.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos {

namespace param_store_test_access {
class ParamStoreTestAccess;
}

/// Move-only, thread-safe synchronous client for base/session parameter data.
class ParamStore {
 public:
  struct DeclarationControl;

  static Result<ParamStore> Open(ClientConfig config = {});
  static Result<ParamStore> Open(std::shared_ptr<Transport> transport, ClientConfig config = {});

  ParamStore(const ParamStore&) = delete;
  ParamStore& operator=(const ParamStore&) = delete;
  ParamStore(ParamStore&& other) noexcept = default;
  ParamStore& operator=(ParamStore&& other) noexcept = default;

  Result<ParamSubscription> Subscribe(std::string_view scope, std::string_view prefix,
                                      ParamCallback callback);

  Result<void> Put(std::string_view scope, std::string_view key, const ParamValue& value);

  template <ParamInput T>
  Result<void> Put(std::string_view scope, std::string_view key, T&& value) {
    auto converted = param_detail::MakeParamValue(std::forward<T>(value));
    if (!converted.IsOk()) return Result<void>::ErrFrom(converted);
    return Put(scope, key, converted.Value());
  }

  Result<void> PutBatch(std::string_view scope, std::span<const BatchEntry> entries);
  Result<void> Delete(std::string_view scope, std::string_view key);

  Result<ParamValue> Get(std::string_view scope, std::string_view key);

  template <SupportedParamType T>
  Result<T> Get(std::string_view scope, std::string_view key) {
    auto value = Get(scope, key);
    if (!value.IsOk()) return Result<T>::ErrFrom(value);
    auto converted = value.Value().template As<T>();
    if (!converted.has_value()) {
      return Result<T>::Err(Status::TypeMismatch, "parameter value type mismatch");
    }
    return Result<T>::Ok(std::move(*converted));
  }

  Result<bool> Contains(std::string_view scope, std::string_view key);
  Result<void> List(std::string_view scope, std::string_view prefix, const ListSink& sink);

 private:
  friend class param_store_test_access::ParamStoreTestAccess;

  ParamStore(std::shared_ptr<Transport> transport, ClientConfig config);

  std::shared_ptr<DeclarationControl> declaration_control_;

  static Result<Scope> ParseAndValidateScope(std::string_view scope);
  static Result<void> ValidateUserKey(std::string_view key);
  static Result<void> ValidateListPrefix(std::string_view prefix);

  std::shared_ptr<Transport> transport_;
  ClientConfig config_;
};

}  // namespace sitos

#endif  // SITOS_PARAM_STORE_HPP
