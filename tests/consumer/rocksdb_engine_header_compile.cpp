// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <type_traits>

#include "sitos/rocksdb_engine.hpp"

static_assert(std::is_destructible_v<sitos::RocksDBEngine>);
static_assert(!std::is_copy_constructible_v<sitos::RocksDBEngine>);

int main() { return 0; }
