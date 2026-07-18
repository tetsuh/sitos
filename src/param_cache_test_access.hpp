// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
#ifndef SITOS_PARAM_CACHE_TEST_ACCESS_HPP
#define SITOS_PARAM_CACHE_TEST_ACCESS_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "sitos/param_cache.hpp"
#include "sitos/param_value.hpp"

namespace sitos::param_cache_test_access {

class ParamCacheTestAccess {
 public:
  static bool IsAttached(const ParamCache& cache);
  static std::size_t Size(const ParamCache& cache);
  static std::optional<ParamValue> Get(const ParamCache& cache, std::string_view key);
  // Precondition: no callback is executing while the hook is being set.
  static void SetCallbackHook(ParamCache& cache, std::function<void()> hook);
  // Precondition: no callback is executing while the hook is being set.
  static void SetMutationHook(ParamCache& cache, std::function<void(std::size_t)> hook);
};

}  // namespace sitos::param_cache_test_access
#endif  // SITOS_PARAM_CACHE_TEST_ACCESS_HPP
