// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "sitos/rocksdb_engine.hpp"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  auto opened = sitos::RocksDBEngine::Open(argv[1]);
  if (!opened.IsOk()) return 3;
  auto engine = std::move(opened).Value();
  if (!engine->Put("durable/alpha",
                   std::vector<std::byte>{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}})) {
    return 4;
  }
  if (!engine->Put("durable/beta", std::vector<std::byte>{std::byte{0x42}})) {
    return 5;
  }
  if (!engine->Put("durable/deleted", std::vector<std::byte>{std::byte{0x11}}) ||
      !engine->Delete("durable/deleted")) {
    return 6;
  }
  if (!engine->Sync().IsOk()) return 7;

  // Deliberately skip every automatic object destructor. The parent process
  // reopens the database and verifies the exact synchronized prefix.
  std::_Exit(EXIT_SUCCESS);
}
