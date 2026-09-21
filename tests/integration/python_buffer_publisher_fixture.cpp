// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#if SITOS_WITH_ROCKSDB
#include "sitos/rocksdb_engine.hpp"
#endif
#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

int main(int argc, char** argv) {
  const std::string prefix = argc > 1 ? argv[1] : "sitos/python-buffer-fixture";
  const std::string sid = argc > 2 ? argv[2] : "python";
  const std::string root = argc > 3 ? argv[3] : "";
  auto transport_result = sitos::OpenZenohTransport();
  if (!transport_result.IsOk()) {
    std::cerr << transport_result.Message() << '\n';
    return 1;
  }
  auto transport = std::move(transport_result).Value();
  sitos::StorageNode node(*transport);
  auto started = node.Start(
      std::make_shared<sitos::InMemoryEngine>(),
      {.prefix = prefix, .durable_buffer_engine_factory = [root](std::string_view sid_value) {
#if SITOS_WITH_ROCKSDB
         if (!root.empty()) {
           auto opened = sitos::RocksDBEngine::Open(root + "/" + std::string(sid_value));
           if (!opened.IsOk()) {
             return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
           }
           return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
               std::move(opened).Value());
         }
#else
         static_cast<void>(sid_value);
#endif
         return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
             std::make_unique<sitos::InMemoryEngine>());
       }});
  if (!started.IsOk() ||
      !node.CreateSession(sid, {.durable_buffers = true, .ephemeral_buffers = true}).IsOk()) {
    std::cerr << "unable to start Python BufferPublisher fixture\n";
    return 1;
  }
  std::cout << "READY\n" << std::flush;
  std::string command;
  std::getline(std::cin, command);
  return 0;
}
