// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PARAM_STORE_TEST_ACCESS_HPP
#define SITOS_PARAM_STORE_TEST_ACCESS_HPP

#include <functional>

#include "sitos/param_store.hpp"

namespace sitos::param_store_test_access {

class ParamStoreTestAccess {
 public:
  static void SetNativeEntryHook(ParamStore& store, std::function<void()> hook);
  static void SetLifecycleHooks(ParamStore& store, std::function<void()> fail_staging_hook,
                                std::function<void()> close_admission_hook,
                                std::function<void()> close_reset_hook);
};

}  // namespace sitos::param_store_test_access

#endif  // SITOS_PARAM_STORE_TEST_ACCESS_HPP
