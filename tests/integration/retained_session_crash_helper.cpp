// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0
//
// Child process for the ADR-0036 catalog crash tests. It runs one scenario on a
// durable root and stops with std::_Exit at the named catalog checkpoint, skipping
// every destructor, so the parent observes the state a process crash leaves.

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "retained_session_support.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/storage_node.hpp"
#include "storage_node_test_access.hpp"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string root = argv[1];
  const std::string scenario = argv[2];

  retained_session_test::CatalogTransport transport;
  sitos::StorageNode node{transport};
  sitos::StorageNodeConfig config;
  config.prefix = "sitos";
  config.durable_root = root;
  if (!node.Start(std::make_shared<sitos::InMemoryEngine>(), config).IsOk()) return 3;
  if (!node.Readiness().ready) return 4;

  std::string exit_point;
  if (scenario == "create-record") exit_point = "create:active_written";
  if (scenario == "retain") exit_point = "retain:retained_written";
  if (scenario == "close-deleting") exit_point = "close:deleting_written";
  if (scenario == "close-removed") exit_point = "close:removed";
  if (!sitos::storage_node_test_access::StorageNodeTestAccess::SetCatalogCheckpoint(
          node, [exit_point](std::string_view point) {
            if (point == exit_point) std::_Exit(EXIT_SUCCESS);
          })) {
    return 5;
  }

  if (scenario == "create-record") {
    static_cast<void>(node.CreateSession("run", {.durable_buffers = true}));
    return 6;  // the checkpoint must have exited
  }
  if (!node.CreateSession("run", {.durable_buffers = true}).IsOk()) return 7;
  transport.PutBuffer("run", "k", {std::byte{7}});
  if (scenario == "active") std::_Exit(EXIT_SUCCESS);
  if (scenario == "retain") {
    static_cast<void>(node.RetainSession("run"));
    return 8;
  }
  if (!node.CreateSession("other", {.durable_buffers = true}).IsOk()) return 9;
  transport.PutBuffer("other", "k", {std::byte{8}});
  if (!node.RetainSession("other").IsOk()) return 10;
  static_cast<void>(node.CloseSession("run"));
  return 11;
}
