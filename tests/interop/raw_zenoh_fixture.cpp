// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_store.hpp"
#include "sitos/status.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"
#include "storage_node_test_access.hpp"

namespace {

using namespace std::chrono_literals;

template <typename T>
void ReportStartupFailure(std::string_view stage, const sitos::Result<T>& result) {
  std::cerr << stage << " failed: status=" << static_cast<int>(result.StatusCode());
  if (!result.Message().empty()) std::cerr << ", message=" << result.Message();
  const auto& cause = result.Error();
  if (cause) {
    std::cerr << ", cause=" << cause.category().name() << ':' << cause.value() << ' '
              << cause.message();
  }
  std::cerr << '\n';
}

class Protocol final {
 public:
  void Reply(std::string_view line) { std::cout << line << std::endl; }

  void Error(std::string_view operation, std::string_view message) {
    std::cout << "ERROR " << operation << " " << message << std::endl;
  }
};

bool PutDpAndConfirm(sitos::ParamStore& store, std::string_view key, double value) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto put = store.Put("base", key, value, sitos::ParamStore::WriteOptions{.ack = false});
    if (!put.IsOk()) return false;
    const auto observed = store.Get<double>("base", key);
    if (observed.IsOk()) return observed.Value() == value;
    if (observed.StatusCode() != sitos::Status::NotFound) return false;
  }
  return false;
}

bool HandlePutDp(std::istringstream& input, sitos::ParamStore& store, Protocol& protocol) {
  std::string key;
  double value = 0.0;
  std::string trailing;
  if (!(input >> key >> value) || (input >> trailing) || !std::isfinite(value)) {
    protocol.Error("PUT_DP", "invalid arguments");
    return true;
  }
  if (!PutDpAndConfirm(store, key, value)) {
    protocol.Error("PUT_DP", "write was not observed before the deadline");
    return true;
  }
  protocol.Reply("PUT_OK " + key);
  return true;
}

bool HandleCreateSession(std::istringstream& input, sitos::StorageNode& node, Protocol& protocol) {
  std::string session_id;
  std::string trailing;
  if (!(input >> session_id) || (input >> trailing)) {
    protocol.Error("CREATE_SESSION", "invalid arguments");
    return true;
  }
  const auto created = node.CreateSession(session_id);
  if (!created.IsOk()) {
    protocol.Error("CREATE_SESSION", created.Message());
    return true;
  }
  protocol.Reply("SESSION_OK " + session_id);
  return true;
}

// Records every base batch sample seen by a C++ Transport subscriber, decoded
// with the public sitos.v1.batch codec.
class BatchObserver final {
 public:
  void Observe(const sitos::TransportSample& sample) {
    std::optional<std::vector<sitos::BatchEntry>> decoded;
    if (sample.kind == sitos::TransportSample::Kind::Put &&
        sample.encoding.id == sitos::Encoding::kSitosV1Batch) {
      decoded = sitos::DecodeBatch(sample.payload);
    }
    {
      std::scoped_lock lock(mutex_);
      observations_.push_back(std::move(decoded));
    }
    changed_.notify_all();
  }

  std::size_t Count() {
    std::scoped_lock lock(mutex_);
    return observations_.size();
  }

  // The first observation recorded after `seen` earlier observations.
  std::optional<std::optional<std::vector<sitos::BatchEntry>>> WaitAfter(std::size_t seen) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, 2s, [&] { return observations_.size() > seen; })) {
      return std::nullopt;
    }
    return observations_[seen];
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::optional<std::vector<sitos::BatchEntry>>> observations_;
};

struct FixtureContext {
  std::shared_ptr<sitos::Transport> transport;
  std::string prefix;
  sitos::ParamStore& store;
  sitos::StorageNode& node;
  Protocol& protocol;
  BatchObserver batch_observer;
  std::optional<sitos::Subscription> batch_subscription;
};

std::optional<unsigned> HexNibble(char digit) {
  if (digit >= '0' && digit <= '9') return static_cast<unsigned>(digit - '0');
  if (digit >= 'a' && digit <= 'f') return static_cast<unsigned>(digit - 'a' + 10);
  return std::nullopt;
}

std::optional<std::vector<std::byte>> DecodeHex(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const auto high = HexNibble(hex[i]);
    const auto low = HexNibble(hex[i + 1]);
    if (!high || !low) return std::nullopt;
    bytes.push_back(static_cast<std::byte>((*high << 4) | *low));
  }
  return bytes;
}

std::string EncodeHex(std::span<const std::byte> bytes) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    hex.push_back(kDigits[value >> 4]);
    hex.push_back(kDigits[value & 0x0f]);
  }
  return hex;
}

// Parses `<key>=<tag>:<hex body>` into a typed batch entry.
std::optional<sitos::BatchEntry> ParseBatchEntry(std::string_view field) {
  const auto equals = field.find('=');
  const auto colon = field.find(':', equals == std::string_view::npos ? 0 : equals);
  if (equals == std::string_view::npos || colon == std::string_view::npos || equals == 0 ||
      colon != equals + 2) {
    return std::nullopt;
  }
  const char tag = field[equals + 1];
  if (tag < '0' || tag > '9') return std::nullopt;
  const auto body = DecodeHex(field.substr(colon + 1));
  if (!body) return std::nullopt;
  auto value = sitos::ParamValue::Decode(static_cast<std::uint8_t>(tag - '0'), *body);
  if (!value) return std::nullopt;
  return sitos::BatchEntry{std::string(field.substr(0, equals)), std::move(*value)};
}

std::string FormatBatch(const std::vector<sitos::BatchEntry>& entries) {
  std::string line = "BATCH_OBSERVED";
  for (const auto& entry : entries) {
    const auto body = entry.value.EncodeBody();
    line += " " + entry.key + "=" + std::to_string(static_cast<unsigned>(entry.value.type())) +
            ":" + EncodeHex(body);
  }
  return line;
}

bool HandleSubscribeBatch(std::istringstream& input, FixtureContext& context) {
  std::string trailing;
  if ((input >> trailing) || context.batch_subscription) {
    context.protocol.Error("SUBSCRIBE_BATCH", "invalid arguments");
    return true;
  }
  auto* observer = &context.batch_observer;
  auto subscription = context.transport->DeclareSubscriber(
      context.prefix + "/base/:batch",
      [observer](const sitos::TransportSample& sample) { observer->Observe(sample); });
  if (!subscription.IsOk()) {
    context.protocol.Error("SUBSCRIBE_BATCH", subscription.Message());
    return true;
  }
  context.batch_subscription.emplace(std::move(subscription).Value());
  context.protocol.Reply("BATCH_SUBSCRIBED");
  return true;
}

bool HandlePutBatch(std::istringstream& input, FixtureContext& context) {
  std::vector<sitos::BatchEntry> entries;
  std::string field;
  while (input >> field) {
    auto entry = ParseBatchEntry(field);
    if (!entry) {
      context.protocol.Error("PUT_BATCH", "invalid entry");
      return true;
    }
    entries.push_back(std::move(*entry));
  }
  if (entries.empty() || !context.batch_subscription) {
    context.protocol.Error("PUT_BATCH", "invalid arguments");
    return true;
  }
  const auto seen = context.batch_observer.Count();
  const auto put =
      context.store.PutBatch("base", entries, sitos::ParamStore::WriteOptions{.ack = false});
  if (!put.IsOk()) {
    context.protocol.Error("PUT_BATCH", put.Message());
    return true;
  }
  const auto observed = context.batch_observer.WaitAfter(seen);
  if (!observed) {
    context.protocol.Error("PUT_BATCH", "C++ subscriber observed no batch before the deadline");
    return true;
  }
  if (!*observed) {
    context.protocol.Error("PUT_BATCH", "C++ subscriber observed a malformed batch");
    return true;
  }
  context.protocol.Reply(FormatBatch(**observed));
  return true;
}

bool HandleAckEntries(std::istringstream& input, FixtureContext& context) {
  std::string trailing;
  const auto count =
      sitos::storage_node_test_access::StorageNodeTestAccess::AckRegistryEntryCount(context.node);
  if ((input >> trailing) || !count) {
    context.protocol.Error("ACK_ENTRIES", "invalid arguments or stopped node");
    return true;
  }
  context.protocol.Reply("ACK_ENTRIES " + std::to_string(*count));
  return true;
}

bool HandleCommand(std::string_view command, FixtureContext& context) {
  std::istringstream input{std::string(command)};
  std::string operation;
  input >> operation;
  auto& protocol = context.protocol;
  if (operation == "PUT_DP") return HandlePutDp(input, context.store, protocol);
  if (operation == "CREATE_SESSION") return HandleCreateSession(input, context.node, protocol);
  if (operation == "SUBSCRIBE_BATCH") return HandleSubscribeBatch(input, context);
  if (operation == "PUT_BATCH") return HandlePutBatch(input, context);
  if (operation == "ACK_ENTRIES") return HandleAckEntries(input, context);
  if (operation == "STOP") {
    protocol.Reply("STOPPED");
    return false;
  }
  protocol.Error(operation.empty() ? "COMMAND" : operation, "unsupported command");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string prefix = argv[1];
  const std::string port = argv[2];
  const std::string config = "{mode: 'peer', listen: {endpoints: ['tcp/127.0.0.1:" + port +
                             "']}, scouting: {multicast: {enabled: false}}}";

  auto transport_result = sitos::OpenZenohTransport(config);
  if (!transport_result.IsOk()) {
    ReportStartupFailure("OpenZenohTransport", transport_result);
    return 3;
  }
  std::shared_ptr<sitos::Transport> transport(std::move(transport_result).Value());
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node(*transport);
  const auto start_result = node.Start(engine, {.prefix = prefix, .log_sink = nullptr});
  if (!start_result.IsOk()) {
    ReportStartupFailure("StorageNode::Start", start_result);
    return 4;
  }

  sitos::ClientConfig client_config;
  client_config.prefix = prefix;
  client_config.query_timeout = 250ms;
  client_config.log_sink = nullptr;
  auto store_result = sitos::ParamStore::Open(transport, std::move(client_config));
  if (!store_result.IsOk()) {
    ReportStartupFailure("ParamStore::Open", store_result);
    node.Stop();
    return 5;
  }
  auto store = std::move(store_result).Value();

  Protocol protocol;
  FixtureContext context{.transport = transport,
                         .prefix = prefix,
                         .store = store,
                         .node = node,
                         .protocol = protocol,
                         .batch_observer = {},
                         .batch_subscription = std::nullopt};
  protocol.Reply("READY " + prefix + " " + port);
  std::string command;
  while (std::getline(std::cin, command) && HandleCommand(command, context)) {
  }
  context.batch_subscription.reset();
  node.Stop();
  return 0;
}
