// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_store.hpp"
#if defined(SITOS_WITH_ROCKSDB)
#include "sitos/rocksdb_engine.hpp"
#endif
#include "sitos/status.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

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
    const auto put = store.Put("base", key, value);
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

bool HandleCreateBufferSession(std::istringstream& input, sitos::StorageNode& node,
                               Protocol& protocol) {
  std::string session_id;
  std::string mode = "both";
  std::string trailing;
  if (!(input >> session_id)) {
    protocol.Error("CREATE_BUFFER_SESSION", "invalid arguments");
    return true;
  }
  if (input >> mode && input >> trailing) {
    protocol.Error("CREATE_BUFFER_SESSION", "invalid arguments");
    return true;
  }
  sitos::SessionOptions options;
  if (mode == "durable") {
    options.durable_buffers = true;
  } else if (mode == "ephemeral") {
    options.ephemeral_buffers = true;
  } else if (mode == "both") {
    options.durable_buffers = true;
    options.ephemeral_buffers = true;
  } else if (mode != "none") {
    protocol.Error("CREATE_BUFFER_SESSION", "invalid mode");
    return true;
  }
  const auto created = node.CreateSession(session_id, options);
  if (!created.IsOk()) {
    protocol.Error("CREATE_BUFFER_SESSION", created.Message());
    return true;
  }
  protocol.Reply("BUFFER_SESSION_OK " + session_id);
  return true;
}

bool HandleCloseSession(std::istringstream& input, sitos::StorageNode& node,
                        const std::filesystem::path& durable_root, Protocol& protocol) {
  std::string session_id;
  std::string trailing;
  if (!(input >> session_id) || (input >> trailing)) {
    protocol.Error("CLOSE_SESSION", "invalid arguments");
    return true;
  }
  const auto closed = node.CloseSession(session_id);
  if (!closed.IsOk()) {
    protocol.Error("CLOSE_SESSION", closed.Message());
    return true;
  }
  std::error_code error;
  std::filesystem::remove_all(durable_root / session_id, error);
  if (error) {
    protocol.Error("CLOSE_SESSION", error.message());
    return true;
  }
  protocol.Reply("CLOSED " + session_id);
  return true;
}

bool HandleCommand(std::string_view command, sitos::ParamStore& store, sitos::StorageNode& node,
                   const std::filesystem::path& durable_root, Protocol& protocol) {
  std::istringstream input{std::string(command)};
  std::string operation;
  input >> operation;
  if (operation == "PUT_DP") return HandlePutDp(input, store, protocol);
  if (operation == "CREATE_SESSION") return HandleCreateSession(input, node, protocol);
  if (operation == "CREATE_BUFFER_SESSION") {
    return HandleCreateBufferSession(input, node, protocol);
  }
  if (operation == "CLOSE_SESSION") {
    return HandleCloseSession(input, node, durable_root, protocol);
  }
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
  const auto durable_root = std::filesystem::temp_directory_path() /
                            ("sitos-buffer-" + std::to_string(std::hash<std::string>{}(prefix)));
  std::error_code cleanup_error;
  std::filesystem::remove_all(durable_root, cleanup_error);
  if (cleanup_error) {
    std::cerr << "durable root cleanup failed: " << cleanup_error.message() << '\n';
    return 6;
  }
  std::filesystem::create_directories(durable_root, cleanup_error);
  if (cleanup_error || !std::filesystem::is_directory(durable_root, cleanup_error) ||
      cleanup_error) {
    std::cerr << "durable root creation failed: "
              << (cleanup_error ? cleanup_error.message() : "not a directory") << '\n';
    return 6;
  }
  sitos::StorageNode node(*transport);
  const auto start_result = node.Start(
      engine, {.prefix = prefix,
               .log_sink = nullptr,
               .durable_buffer_engine_factory = [durable_root](std::string_view sid) {
#if defined(SITOS_WITH_ROCKSDB)
                 auto opened =
                     sitos::RocksDBEngine::Open((durable_root / std::string(sid)).string());
                 if (!opened.IsOk()) {
                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
                 }
                 return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                     std::unique_ptr<sitos::StorageEngine>(std::move(opened).Value()));
#else
                 static_cast<void>(sid);
                 return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                     std::make_unique<sitos::InMemoryEngine>());
#endif
               }});
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
  protocol.Reply("READY " + prefix + " " + port);
  std::string command;
  while (std::getline(std::cin, command) &&
         HandleCommand(command, store, node, durable_root, protocol)) {
  }
  node.Stop();
  cleanup_error.clear();
  std::filesystem::remove_all(durable_root, cleanup_error);
  if (cleanup_error) {
    std::cerr << "durable root final cleanup failed: " << cleanup_error.message() << '\n';
    return 6;
  }
  return 0;
}
