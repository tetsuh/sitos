// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_value.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>

int main(int argc, char** argv) {
  const std::string prefix = argc > 1 ? argv[1] : "sitos/python_fixture";
  const std::string port = argc > 2 ? argv[2] : "17447";
  const std::string config =
      "{mode: 'peer', listen: {endpoints: ['tcp/127.0.0.1:" + port + "']}}";
  auto transport_result = sitos::OpenZenohTransport(config);
  if (!transport_result.IsOk()) return 2;
  auto transport = std::move(transport_result).Value();
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node(*transport);
  if (!node.Start(engine, {.prefix = prefix}).IsOk()) return 3;

  std::mutex mutex;
  std::condition_variable condition;
  bool release_delayed = false;
  auto delayed = transport->DeclareQueryable(
      prefix + "/base/__python_gil_delay__", [&](sitos::TransportQuery& query) {
        if (query.keyexpr != prefix + "/base/__python_gil_delay__") return;
        {
          std::lock_guard lock(mutex);
          std::cout << "DELAYED" << std::endl;
        }
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release_delayed; });
        const std::byte body[] = {std::byte{1}, std::byte{7}, std::byte{0}, std::byte{0},
                                  std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                  std::byte{0}};
        static_cast<void>(query.Reply(prefix + "/base/__python_gil_delay__", body,
                                      sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}));
      });
  if (!delayed.IsOk()) {
    node.Stop();
    return 4;
  }
  std::cout << "READY " << prefix << " " << port << std::endl;
  std::string command;
  while (std::getline(std::cin, command)) {
    if (command == "REPLY") {
      {
        std::lock_guard lock(mutex);
        release_delayed = true;
      }
      condition.notify_all();
    }
    if (command == "STOP") {
      {
        std::lock_guard lock(mutex);
        release_delayed = true;
      }
      condition.notify_all();
      break;
    }
  }
  node.Stop();
  return 0;
}
