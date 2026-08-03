// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

// This tutorial intentionally uses only the installed public umbrella header.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <sitos/sitos.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

template <typename T>
void ReportFailure(std::string_view operation, const sitos::Result<T>& result) {
  std::cerr << operation << " failed (status=" << static_cast<int>(result.StatusCode());
  if (!result.Message().empty()) std::cerr << ", " << result.Message();
  if (!result.IsOk()) {
    const auto& cause = result.Error();
    if (cause) std::cerr << ", cause=" << cause.category().name() << ':' << cause.value();
  }
  std::cerr << ")\n";
}

std::string RunToken() {
  const auto ticks =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto random = static_cast<std::uint64_t>(std::random_device{}());
  return std::to_string(ticks) + "_" + std::to_string(random);
}

bool IsValidIdentity(const std::string& prefix, const std::string& sid) {
  return sitos::IsValidPrefix(prefix) && sitos::IsValidSessionId(sid);
}
}  // namespace

int main() {
  const std::string token = RunToken();
  const std::string prefix = "sitos/quickstart_" + token;
  const std::string sid = "quickstart_" + token;
  if (!IsValidIdentity(prefix, sid)) {
    std::cerr << "generated example identity was invalid\n";
    return 1;
  }

  constexpr std::int64_t expected_scalar = 42;
  const std::vector<std::byte> expected_lut{std::byte{0x00}, std::byte{0x11}, std::byte{0x7f},
                                            std::byte{0xff}};

  // Open one Transport. StorageNode borrows it, while the two client objects
  // share it; using the injected Open overloads avoids opening extra sessions.
  auto opened_transport = sitos::OpenZenohTransport();
  if (!opened_transport.IsOk()) {
    ReportFailure("OpenZenohTransport", opened_transport);
    return 1;
  }
  std::shared_ptr<sitos::Transport> transport(std::move(opened_transport).Value());
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node(*transport);
  auto started = node.Start(engine, {.prefix = prefix, .log_sink = nullptr});
  if (!started.IsOk()) {
    ReportFailure("StorageNode::Start", started);
    return 1;
  }

  sitos::ClientConfig client_config;
  client_config.prefix = prefix;
  client_config.query_timeout = 200ms;
  client_config.log_sink = nullptr;
  auto opened_store = sitos::ParamStore::Open(transport, client_config);
  if (!opened_store.IsOk()) {
    ReportFailure("ParamStore::Open", opened_store);
    node.Stop();
    return 1;
  }
  auto store = std::move(opened_store).Value();
  auto opened_cache = sitos::ParamCache::Open(transport, client_config);
  if (!opened_cache.IsOk()) {
    ReportFailure("ParamCache::Open", opened_cache);
    node.Stop();
    return 1;
  }
  auto cache = std::move(opened_cache).Value();

  // Put returns after transport submission, not after the node has applied the
  // value. Submit each value once, then observe both exact values before taking
  // the session snapshot below.
  auto scalar_put = store.Put("base", "answer", expected_scalar);
  if (!scalar_put.IsOk()) {
    ReportFailure("ParamStore::Put(answer)", scalar_put);
    node.Stop();
    return 1;
  }
  auto lut_put = store.Put("base", "lut", expected_lut);
  if (!lut_put.IsOk()) {
    ReportFailure("ParamStore::Put(lut)", lut_put);
    node.Stop();
    return 1;
  }

  bool observed = false;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto scalar = store.Get<std::int64_t>("base", "answer");
    auto lut = store.Get<std::vector<std::byte>>("base", "lut");
    if (scalar.IsOk() && lut.IsOk()) {
      observed = scalar.Value() == expected_scalar && lut.Value() == expected_lut;
      break;
    }
    if ((!scalar.IsOk() && scalar.StatusCode() != sitos::Status::NotFound) ||
        (!lut.IsOk() && lut.StatusCode() != sitos::Status::NotFound)) {
      if (!scalar.IsOk() && scalar.StatusCode() != sitos::Status::NotFound)
        ReportFailure("ParamStore::Get(answer)", scalar);
      if (!lut.IsOk() && lut.StatusCode() != sitos::Status::NotFound)
        ReportFailure("ParamStore::Get(lut)", lut);
      break;
    }
  }
  if (!observed) {
    std::cerr << "base values were not observed exactly before the deadline\n";
    node.Stop();
    return 1;
  }

  auto created = node.CreateSession(sid);
  if (!created.IsOk()) {
    ReportFailure("StorageNode::CreateSession", created);
    node.Stop();
    return 1;
  }
  auto attached = cache.Attach(sid);
  if (!attached.IsOk()) {
    ReportFailure("ParamCache::Attach", attached);
    auto closed = node.CloseSession(sid);
    if (!closed.IsOk()) ReportFailure("StorageNode::CloseSession", closed);
    node.Stop();
    return 1;
  }

  int exit_code = 0;
  auto cached_scalar = cache.Get<std::int64_t>("answer");
  if (!cached_scalar.IsOk()) {
    ReportFailure("ParamCache::Get(answer)", cached_scalar);
    exit_code = 1;
  } else if (cached_scalar.Value() != expected_scalar) {
    std::cerr << "cached scalar did not match\n";
    exit_code = 1;
  }
  auto lut_result = cache.GetSpan<std::byte>("lut");
  if (!lut_result.IsOk()) {
    ReportFailure("ParamCache::GetSpan(lut)", lut_result);
    exit_code = 1;
  } else {
    // SpanHandle owns the ParamValue through keepalive. Never retain the span
    // after this handle is destroyed, even though the bytes are read-only.
    auto lut_handle = std::move(lut_result).Value();
    if (lut_handle.span.size() != expected_lut.size() ||
        !std::equal(lut_handle.span.begin(), lut_handle.span.end(), expected_lut.begin())) {
      std::cerr << "cached LUT did not match byte-for-byte\n";
      exit_code = 1;
    }
  }

  // Keep this order: Detach first, check CloseSession, and stop the node last.
  cache.Detach();
  auto closed = node.CloseSession(sid);
  if (!closed.IsOk()) {
    ReportFailure("StorageNode::CloseSession", closed);
    exit_code = 1;
  }
  node.Stop();
  return exit_code;
}
