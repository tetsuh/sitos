// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_value.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

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
  // ADR-0036 §D6: fixed session keys that answer with typed query error replies.
  auto state_lost = transport->DeclareQueryable(
      prefix + "/session/typed-state-lost/value", [](sitos::TransportQuery& query) {
        static_cast<void>(query.ReplyError(sitos::Status::StateLost));
      });
  auto catalog_unavailable = transport->DeclareQueryable(
      prefix + "/session/typed-catalog-unavailable/**", [](sitos::TransportQuery& query) {
        static_cast<void>(query.ReplyError(sitos::Status::CatalogUnavailable));
      });
  if (!state_lost.IsOk() || !catalog_unavailable.IsOk()) {
    node.Stop();
    return 6;
  }
  auto batch_result = transport->DeclareSubscriber(
      prefix + "/base/:batch", [&](const sitos::TransportSample& sample) {
        if (sample.kind != sitos::TransportSample::Kind::Put) return;
        const auto entries = sitos::DecodeBatch(sample.payload);
        if (!entries.has_value() || entries->size() != 2 || (*entries)[0].key != "dup" ||
            (*entries)[1].key != "dup") {
          return;
        }
        std::lock_guard lock(mutex);
        std::cout << "BATCH " << entries->size();
        for (const auto& entry : *entries) {
          const auto value = entry.value.As<std::int64_t>();
          std::cout << " " << value.value_or(-1);
        }
        std::cout << std::endl;
      });
  if (!batch_result.IsOk()) {
    node.Stop();
    return 5;
  }
  auto batch_subscription = std::move(batch_result).Value();
  std::cout << "READY " << prefix << " " << port << std::endl;
  std::string command;
  // Publishes "STR <path> <text>" or a malformed UTF-8 STR ("MALFORMED_STR <path>"); a path
  // ending in ":batch" carries the malformed value between two valid batch entries.
  const auto publish = [&](const std::string& path, const std::string& text) {
    const std::string key = prefix + "/" + path;
    if (path.ends_with(":batch")) {
      const std::vector<sitos::BatchEntry> entries{
          {"malformed/a", sitos::ParamValue(std::string("ok-a"))},
          {"malformed/bad", sitos::ParamValue(text)},
          {"malformed/c", sitos::ParamValue(std::string("ok-c"))}};
      return transport->Put(key, sitos::EncodeBatch(entries),
                            sitos::Encoding{std::string(sitos::Encoding::kSitosV1Batch)}, {});
    }
    return transport->Put(key, sitos::ParamValue(text).Encode(),
                          sitos::Encoding{std::string(sitos::Encoding::kSitosV1)}, {});
  };
  while (std::getline(std::cin, command)) {
    if (command.starts_with("STR ") || command.starts_with("MALFORMED_STR ")) {
      const bool malformed = command.starts_with("MALFORMED_STR ");
      const std::string rest = command.substr(command.find(' ') + 1);
      const auto space = rest.find(' ');
      const std::string path = malformed ? rest : rest.substr(0, space);
      const std::size_t text_start = space == std::string::npos ? rest.size() : space + 1;
      const std::string text = malformed ? std::string("\xff\xfe") : rest.substr(text_start);
      const bool ok = publish(path, text).IsOk();
      std::lock_guard lock(mutex);
      std::cout << (ok ? "PUBLISHED" : "PUBLISH_FAILED") << std::endl;
      continue;
    }
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
