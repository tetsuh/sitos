// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared support for the ADR-0036 retained-session catalog tests: an in-process
// transport that routes typed error replies, temporary durable roots, and small
// query helpers.

#ifndef SITOS_TESTS_INTEGRATION_RETAINED_SESSION_SUPPORT_HPP
#define SITOS_TESTS_INTEGRATION_RETAINED_SESSION_SUPPORT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sitos/param_value.hpp"
#include "sitos/status.hpp"
#include "sitos/transport.hpp"
#include "transport/declaration_handle_test_access.hpp"

namespace retained_session_test {

/// Delivers samples and queries synchronously to the one declared StorageNode.
/// A typed error reply ends Get with its Status, as the Zenoh client does.
class CatalogTransport final : public sitos::Transport {
 public:
  sitos::Result<void> Put(std::string_view, std::span<const std::byte>, sitos::Encoding,
                          sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    std::optional<sitos::Status> refused;
    auto query = sitos::TransportQuery::ForTesting(
        [&](std::string_view key, std::span<const std::byte> payload, sitos::Encoding encoding) {
          if (refused.has_value()) return sitos::Result<void>::Ok();
          sink(key, payload, std::move(encoding));
          return sitos::Result<void>::Ok();
        },
        [&](sitos::Status status) {
          if (!refused.has_value()) refused = status;
          return sitos::Result<void>::Ok();
        });
    query.keyexpr = std::string(keyexpr);
    std::function<void(sitos::TransportQuery&)> queryable;
    {
      std::scoped_lock lock(mutex_);
      queryable = queryable_;
    }
    if (queryable) queryable(query);
    if (refused.has_value()) {
      return sitos::Result<void>::Err(*refused, "storage node refused the query");
    }
    return sitos::Result<void>::Ok();
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)> callback) override {
    {
      std::scoped_lock lock(mutex_);
      subscriber_ = std::move(callback);
    }
    return sitos::Result<sitos::Subscription>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeSubscription([this] {
          std::scoped_lock lock(mutex_);
          subscriber_ = nullptr;
        }));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)> callback) override {
    {
      std::scoped_lock lock(mutex_);
      queryable_ = std::move(callback);
    }
    return sitos::Result<sitos::Queryable>::Ok(
        sitos::transport_test_access::DeclarationHandleTestAccess::MakeQueryable([this] {
          std::scoped_lock lock(mutex_);
          queryable_ = nullptr;
        }));
  }

  void PutEphemeral(const std::string& sid, const std::string& key, std::vector<std::byte> bytes) {
    Deliver(sitos::TransportSample{"sitos/buffers/" + sid + "/ephemeral/" + key,
                                   bytes,
                                   {"zenoh/bytes"},
                                   {},
                                   sitos::TransportSample::Kind::Put});
  }

  void PutBuffer(const std::string& sid, const std::string& key, std::vector<std::byte> bytes) {
    Deliver(sitos::TransportSample{"sitos/buffers/" + sid + "/durable/" + key,
                                   bytes,
                                   {"zenoh/bytes"},
                                   {},
                                   sitos::TransportSample::Kind::Put});
  }

  void PutParameter(const std::string& scope, const std::string& key, std::int64_t value) {
    const auto payload = sitos::ParamValue(value).Encode();
    Deliver(sitos::TransportSample{"sitos/" + scope + "/" + key,
                                   payload,
                                   {std::string(sitos::Encoding::kSitosV1)},
                                   {},
                                   sitos::TransportSample::Kind::Put});
  }

  void Deliver(const sitos::TransportSample& sample) {
    std::function<void(const sitos::TransportSample&)> subscriber;
    {
      std::scoped_lock lock(mutex_);
      subscriber = subscriber_;
    }
    if (subscriber) subscriber(sample);
  }

 private:
  std::mutex mutex_;
  std::function<void(const sitos::TransportSample&)> subscriber_;
  std::function<void(sitos::TransportQuery&)> queryable_;
};

struct QueryOutcome {
  sitos::Result<void> result = sitos::Result<void>::Ok();
  std::vector<std::pair<std::string, std::vector<std::byte>>> replies;
};

inline QueryOutcome Query(sitos::Transport& transport, const std::string& keyexpr) {
  QueryOutcome outcome;
  outcome.result = transport.Get(
      keyexpr,
      [&outcome](std::string_view key, std::span<const std::byte> payload, sitos::Encoding) {
        outcome.replies.emplace_back(std::string(key),
                                     std::vector<std::byte>(payload.begin(), payload.end()));
        return true;
      },
      std::chrono::milliseconds(1000));
  return outcome;
}

/// Returns the state field of meta/session/<sid>, or nullopt for zero replies.
inline std::optional<std::string> MetaState(sitos::Transport& transport, const std::string& sid) {
  const auto outcome = Query(transport, "sitos/meta/session/" + sid);
  if (!outcome.result.IsOk() || outcome.replies.empty()) return std::nullopt;
  const auto decoded = sitos::ParamValue::Decode(outcome.replies.front().second);
  if (!decoded.has_value()) return std::nullopt;
  const auto json = decoded->As<std::string>();
  if (!json.has_value()) return std::nullopt;
  constexpr std::string_view kField = R"("state":")";
  const auto start = json->find(kField);
  if (start == std::string::npos) return std::nullopt;
  const auto value = start + kField.size();
  const auto end = json->find('"', value);
  if (end == std::string::npos) return std::nullopt;
  return json->substr(value, end - value);
}

class ScopedRoot {
 public:
  explicit ScopedRoot(std::string_view label) {
    static std::atomic<unsigned int> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
                             ("sitos-retained-" + std::string(label) + "-" + std::to_string(stamp) +
                              "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error != std::errc::file_exists) throw std::system_error(error, "create durable root");
    }
    throw std::runtime_error("unable to allocate a temporary durable root");
  }
  ~ScopedRoot() {
    std::error_code error;
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    std::filesystem::remove_all(path_, error);
  }
  ScopedRoot(const ScopedRoot&) = delete;
  ScopedRoot& operator=(const ScopedRoot&) = delete;
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace retained_session_test

#endif  // SITOS_TESTS_INTEGRATION_RETAINED_SESSION_SUPPORT_HPP
