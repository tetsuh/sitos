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

#include "sitos/result.hpp"

namespace sitos {

namespace transport_test_access {
class SubscriptionTestAccess;
class DeclarationHandleTestAccess;
}

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
  ///
  /// `timeout` must be strictly positive. Success returns only after the
  /// native reply closure has dropped and every sink invocation for this
  /// request has finished; no sink runs after this method returns. Zero replies
  /// are successful transport completion. The sink is serialized per request,
  /// receives each concrete key at most once, and may return false to suppress
  /// later delivery while the method still waits for completion. Sink views are
  /// valid only for the callback. A sink must not recursively call blocking
  /// Get on the same Transport, but may call nonblocking Put or Delete.
  /// An Error result does not imply the sink was never invoked: a reply-
  /// processing failure can occur after earlier concrete keys were already
  /// delivered, so callers must not treat Error as "no data was seen".
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

namespace sitos {

/// Opens a zenoh transport using an optional complete JSON5 configuration.
/// An absent configuration selects the zenoh default configuration.
Result<std::unique_ptr<Transport>> OpenZenohTransport(
    std::optional<std::string_view> config_json = std::nullopt);

/// Compatibility factory that returns nullptr when opening fails.
std::unique_ptr<Transport> MakeZenohTransport();
}  // namespace sitos

#endif  // SITOS_TRANSPORT_HPP
