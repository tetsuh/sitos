// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef SITOS_PARAM_CACHE_HPP
#define SITOS_PARAM_CACHE_HPP

#include <memory>
#include <string_view>

#include "sitos/client_config.hpp"
#include "sitos/result.hpp"
#include "sitos/transport.hpp"

namespace sitos {

namespace param_cache_test_access {
class ParamCacheTestAccess;
}

namespace param_cache_detail {
struct Access;
}

/// Subscriber-side cache lifecycle. Read and write APIs are provided by later issues.
class ParamCache {
 public:
  static Result<ParamCache> Open(ClientConfig config = {});
  static Result<ParamCache> Open(std::shared_ptr<Transport> transport,
                                 ClientConfig config = {});

  ~ParamCache();
  ParamCache(const ParamCache&) = delete;
  ParamCache& operator=(const ParamCache&) = delete;
  ParamCache(ParamCache&&) noexcept;
  ParamCache& operator=(ParamCache&&) noexcept;

  Result<void> Attach(std::string_view sid);
  void Detach() noexcept;

 private:
  struct Impl;
  friend struct param_cache_detail::Access;
  friend class param_cache_test_access::ParamCacheTestAccess;
  explicit ParamCache(std::shared_ptr<Transport> transport, ClientConfig config);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sitos

#endif  // SITOS_PARAM_CACHE_HPP
