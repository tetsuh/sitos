// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Transport abstraction — hides zenoh-specific types and API from the rest of
// the codebase. See docs/09_dependency_policy.md §3 for the design rationale.

#ifndef SITOS_TRANSPORT_HPP
#define SITOS_TRANSPORT_HPP

#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sitos {

namespace transport_test_access {
class SubscriptionTestAccess;
class DeclarationHandleTestAccess;
}

/// A simple result type that holds either a value or an error code.
/// Modeled after a minimal subset of std::expected (C++23).
template <typename T>
class Result {
 public:
  static Result Ok(T value) {
    Result r;
    r.value_ = std::move(value);
    return r;
  }

  static Result Err(std::error_code ec) {
    Result r;
    r.error_ = ec;
    return r;
  }

  bool IsOk() const { return !error_.has_value(); }
  explicit operator bool() const { return IsOk(); }

  /// Requires IsOk().
  const T& Value() const & {
    assert(IsOk());
    return *value_;
  }
  /// Requires IsOk().
  T& Value() & {
    assert(IsOk());
    return *value_;
  }
  T&& Value() && {
    assert(IsOk());
    return std::move(*value_);
  }

  /// Requires !IsOk().
  const std::error_code& Error() const {
    assert(!IsOk());
    return *error_;
  }

 private:
  Result() = default;
  std::optional<T> value_;
  std::optional<std::error_code> error_;
};

template <>
class Result<void> {
 public:
  static Result Ok() {
    Result r;
    return r;
  }

  static Result Err(std::error_code ec) {
    Result r;
    r.error_ = ec;
    return r;
  }

  bool IsOk() const { return !error_.has_value(); }
  explicit operator bool() const { return IsOk(); }

  /// Requires !IsOk().
  const std::error_code& Error() const {
    assert(!IsOk());
    return *error_;
  }

 private:
  Result() = default;
  std::optional<std::error_code> error_;
};

/// Transport-independent schema identifiers for encoded payloads.
///
/// Transport adapters map these identifiers to their native wire Encoding.
struct Encoding {
  /// The well-known encoding for single-value sitos payloads.
  static constexpr std::string_view kSitosV1 = "sitos.v1";
  /// The well-known encoding for batch sitos payloads.
  static constexpr std::string_view kSitosV1Batch = "sitos.v1.batch";

  std::string id;
};

/// Options for put/delete operations.
struct PutOptions {
  /// When set, the transport will report the ack token back via
  /// TransportSample::ack_token so that higher layers can confirm delivery.
  bool ack = false;
};

/// A sample received from a subscriber or query.
struct TransportSample {
  enum class Kind { Put, Delete };

  std::string key;
  /// Non-owning payload valid only for the callback that receives this sample.
  std::span<const std::byte> payload;
  Encoding encoding;
  std::optional<std::string> ack_token;
  Kind kind;
};

/// A query received from the transport layer. It is valid only for the
/// duration of the queryable callback and must not be retained.
struct TransportQuery {
  std::string keyexpr;

  using ReplyHandler =
      std::function<Result<void>(std::string_view, std::span<const std::byte>, Encoding)>;

  TransportQuery();
  ~TransportQuery();

  /// Creates a query with an in-process reply handler for deterministic tests.
  static TransportQuery ForTesting(ReplyHandler handler) {
    return TransportQuery(std::move(handler));
  }

  TransportQuery(TransportQuery&&) = delete;
  TransportQuery& operator=(TransportQuery&&) = delete;
  TransportQuery(const TransportQuery&) = delete;
  TransportQuery& operator=(const TransportQuery&) = delete;

  Result<void> Reply(std::string_view key, std::span<const std::byte> payload,
                     Encoding encoding);

 private:
  explicit TransportQuery(ReplyHandler handler);

  friend class ZenohTransport;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ReplyHandler test_reply_handler_;
};

/// An active subscription handle. The subscription is cancelled when this
/// object is destroyed.
class Subscription {
 public:
  Subscription();
  ~Subscription();
  Subscription(Subscription&& other) noexcept;
  Subscription& operator=(Subscription&& other) noexcept;

  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;

 private:
  friend class ZenohTransport;
  friend class transport_test_access::SubscriptionTestAccess;
  friend class transport_test_access::DeclarationHandleTestAccess;
  explicit Subscription(std::function<void()> reset_handler);
  struct Impl;
  void Reset() noexcept;
  std::unique_ptr<Impl> impl_;
  std::function<void()> reset_handler_;
};

/// An active queryable handle. The queryable is undeclared when this object
/// is destroyed.
class Queryable {
 public:
  Queryable();
  ~Queryable();
  Queryable(Queryable&& other) noexcept;
  Queryable& operator=(Queryable&& other) noexcept;

  Queryable(const Queryable&) = delete;
  Queryable& operator=(const Queryable&) = delete;

 private:
  friend class ZenohTransport;
  friend class transport_test_access::DeclarationHandleTestAccess;
  explicit Queryable(std::function<void()> reset_handler);
  struct Impl;
  void Reset() noexcept;

  std::unique_ptr<Impl> impl_;
  std::function<void()> reset_handler_;
};

/// Transport is the abstract interface that hides the underlying pub/sub
/// library (zenoh). All higher-level components (StorageNode, ParamStore,
/// ParamCache) depend only on this interface.
class Transport {
 public:
  virtual ~Transport() = default;

  /// Put a value at the given key expression.
  virtual Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                           Encoding encoding, PutOptions options) = 0;

  /// Delete the value at the given key expression.
  virtual Result<void> Delete(std::string_view key, PutOptions options) = 0;

  /// Query results for the given key expression.
  /// The sink is called once per matching key. Return false from the sink to
  /// stop early.
  using QueryResultSink =
      std::function<bool(std::string_view key, std::span<const std::byte> payload,
                         Encoding encoding)>;
  virtual Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                           std::chrono::milliseconds timeout) = 0;

  /// Declare a subscriber that receives put/delete samples under keyexpr.
  virtual Result<Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const TransportSample&)> callback) = 0;

  /// Declare a queryable that answers queries under keyexpr.
  virtual Result<Queryable> DeclareQueryable(
      std::string_view keyexpr,
      std::function<void(TransportQuery&)> callback) = 0;
};

}  // namespace sitos

/// Factory function for the default zenoh-based transport.
/// Returns nullptr when zenoh support is disabled or the zenoh session cannot be opened.
namespace sitos {
std::unique_ptr<Transport> MakeZenohTransport();
}  // namespace sitos

#endif  // SITOS_TRANSPORT_HPP
