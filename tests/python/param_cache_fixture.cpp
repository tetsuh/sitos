// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sitos/batch.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/param_store.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

namespace {

using namespace std::chrono_literals;

class Protocol {
 public:
  void Write(const std::string& line) {
    std::lock_guard lock(mutex_);
    std::cout << line << std::endl;
  }

 private:
  std::mutex mutex_;
};

class SnapshotBarrierTransport final : public sitos::Transport {
 public:
  SnapshotBarrierTransport(std::shared_ptr<sitos::Transport> inner, std::string snapshot_query,
                           std::string batch_key, Protocol& protocol)
      : inner_(std::move(inner)),
        snapshot_query_(std::move(snapshot_query)),
        batch_key_(std::move(batch_key)),
        protocol_(protocol) {}

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    return inner_->Put(key, payload, std::move(encoding), options);
  }

  sitos::Result<void> Delete(std::string_view key, sitos::PutOptions options) override {
    return inner_->Delete(key, options);
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    return inner_->Get(keyexpr, sink, timeout);
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    auto wrapped = [this, callback = std::move(callback)](const sitos::TransportSample& sample) {
      callback(sample);
      if (sample.kind != sitos::TransportSample::Kind::Put || sample.key != batch_key_) return;
      const auto entries = sitos::DecodeBatch(sample.payload);
      if (!entries.has_value()) return;
      std::vector<std::int64_t> values;
      values.reserve(entries->size());
      for (const auto& entry : *entries) {
        const auto value = entry.value.As<std::int64_t>();
        if (value.has_value()) values.push_back(*value);
      }
      {
        std::lock_guard lock(mutex_);
        ++batch_count_;
        last_batch_size_ = entries->size();
        last_batch_values_ = std::move(values);
      }
      batch_condition_.notify_all();
    };
    return inner_->DeclareSubscriber(keyexpr, std::move(wrapped));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view keyexpr, std::function<void(sitos::TransportQuery&)> callback) override {
    auto wrapped = [this, callback = std::move(callback)](sitos::TransportQuery& query) {
      bool should_block = false;
      {
        std::lock_guard lock(mutex_);
        if (armed_ && query.keyexpr == snapshot_query_) {
          armed_ = false;
          entered_ = true;
          should_block = true;
        }
      }
      if (should_block) {
        protocol_.Write("SNAPSHOT_ENTERED");
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return released_; });
      }
      callback(query);
    };
    return inner_->DeclareQueryable(keyexpr, std::move(wrapped));
  }

  bool Arm() {
    std::lock_guard lock(mutex_);
    if (entered_ && !released_) return false;
    armed_ = true;
    entered_ = false;
    released_ = false;
    return true;
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  std::string BatchStatus() const {
    std::lock_guard lock(mutex_);
    return BatchStatusLocked();
  }

  std::string WaitForBatchAfter(std::size_t previous) {
    std::unique_lock lock(mutex_);
    if (!batch_condition_.wait_for(lock, 5s,
                                   [this, previous] { return batch_count_ > previous; })) {
      return "ERROR batch timeout";
    }
    return BatchStatusLocked();
  }

 private:
  std::string BatchStatusLocked() const {
    std::ostringstream line;
    line << "BATCH " << batch_count_ << " " << last_batch_size_;
    for (const auto value : last_batch_values_) line << " " << value;
    return line.str();
  }

  std::shared_ptr<sitos::Transport> inner_;
  const std::string snapshot_query_;
  const std::string batch_key_;
  Protocol& protocol_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable batch_condition_;
  bool armed_ = false;
  bool entered_ = false;
  bool released_ = false;
  std::size_t batch_count_ = 0;
  std::size_t last_batch_size_ = 0;
  std::vector<std::int64_t> last_batch_values_;
};

class PeerTrackingTransport final : public sitos::Transport {
 public:
  explicit PeerTrackingTransport(std::shared_ptr<sitos::Transport> inner)
      : inner_(std::move(inner)) {}

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte> payload,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    return inner_->Put(key, payload, std::move(encoding), options);
  }

  sitos::Result<void> Delete(std::string_view key, sitos::PutOptions options) override {
    return inner_->Delete(key, options);
  }

  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    return inner_->Get(keyexpr, sink, timeout);
  }

  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view keyexpr,
      std::function<void(const sitos::TransportSample&)> callback) override {
    auto wrapped = [this, callback = std::move(callback)](const sitos::TransportSample& sample) {
      callback(sample);
      {
        std::lock_guard lock(mutex_);
        ++callback_count_;
      }
      condition_.notify_all();
    };
    return inner_->DeclareSubscriber(keyexpr, std::move(wrapped));
  }

  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view keyexpr, std::function<void(sitos::TransportQuery&)> callback) override {
    return inner_->DeclareQueryable(keyexpr, std::move(callback));
  }

  bool WaitForValue(sitos::ParamCache& cache, std::string_view key, std::int64_t expected) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s, [&cache, key, expected] {
      const auto value = cache.Get<std::int64_t>(key);
      return value.IsOk() && value.Value() == expected;
    });
  }

 private:
  std::shared_ptr<sitos::Transport> inner_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t callback_count_ = 0;
};

bool IsAbsent(sitos::ParamStore& store, std::string_view scope, std::string_view key) {
  const auto result = store.Contains(scope, key);
  return result.IsOk() && !result.Value();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string prefix = argc > 1 ? argv[1] : "sitos/python_cache_fixture";
  const std::string port = argc > 2 ? argv[2] : "17448";
  const std::string sid = argc > 3 ? argv[3] : "s1";
  const std::string config_json =
      "{mode: 'peer', listen: {endpoints: ['tcp/127.0.0.1:" + port + "']}}";
  auto opened = sitos::OpenZenohTransport(config_json);
  if (!opened.IsOk()) return 2;
  std::shared_ptr<sitos::Transport> raw(std::move(opened).Value());

  Protocol protocol;
  const std::string snapshot_query = prefix + "/snap/" + sid + "/**";
  const std::string batch_key = prefix + "/session/" + sid + "/:batch";
  SnapshotBarrierTransport node_transport(raw, snapshot_query, batch_key, protocol);
  auto peer_transport = std::make_shared<PeerTrackingTransport>(raw);
  auto engine = std::make_shared<sitos::InMemoryEngine>();
  sitos::StorageNode node(node_transport);
  if (!node.Start(engine, {.prefix = prefix}).IsOk()) return 3;
  if (!node.CreateSession(sid).IsOk() || !node.CreateSession("s2").IsOk()) {
    node.Stop();
    return 4;
  }

  sitos::ClientConfig config;
  config.prefix = prefix;
  config.query_timeout = 5s;
  auto store_result = sitos::ParamStore::Open(raw, config);
  if (!store_result.IsOk()) {
    node.Stop();
    return 5;
  }
  auto store = std::move(store_result).Value();
  auto peer_result = sitos::ParamCache::Open(peer_transport, config);
  if (!peer_result.IsOk()) {
    node.Stop();
    return 6;
  }
  auto peer = std::move(peer_result).Value();
  if (!peer.Attach(sid).IsOk()) {
    node.Stop();
    return 7;
  }

  protocol.Write("READY " + prefix + " " + port + " " + sid);
  std::string command;
  while (std::getline(std::cin, command)) {
    if (command == "ARM_SNAPSHOT") {
      protocol.Write(node_transport.Arm() ? "ARMED" : "ERROR barrier active");
    } else if (command == "PUT_DURING_ATTACH") {
      const auto result = store.Put("session/" + sid, "during_attach", std::int64_t{7});
      protocol.Write(result.IsOk() ? "DELTA_SUBMITTED" : "ERROR delta submission");
    } else if (command == "RELEASE_SNAPSHOT") {
      node_transport.Release();
      protocol.Write("SNAPSHOT_RELEASED");
    } else if (command == "BATCH_STATUS") {
      protocol.Write(node_transport.BatchStatus());
    } else if (command.starts_with("WAIT_BATCH ")) {
      const auto previous =
          static_cast<std::size_t>(std::stoull(command.substr(std::string("WAIT_BATCH ").size())));
      protocol.Write(node_transport.WaitForBatchAfter(previous));
    } else if (command.starts_with("WAIT_PEER ")) {
      std::istringstream fields(command);
      std::string name;
      std::string key;
      std::int64_t expected = 0;
      fields >> name >> key >> expected;
      if (fields && peer_transport->WaitForValue(peer, key, expected)) {
        protocol.Write("PEER_OBSERVED " + key + " " + std::to_string(expected));
      } else {
        protocol.Write("ERROR peer timeout");
      }
    } else if (command.starts_with("CHECK_ISOLATION ")) {
      const std::string key = command.substr(std::string("CHECK_ISOLATION ").size());
      if (IsAbsent(store, "base", key) && IsAbsent(store, "session/s2", key)) {
        protocol.Write("ISOLATED " + key);
      } else {
        protocol.Write("ERROR isolation");
      }
    } else if (command == "STOP") {
      node_transport.Release();
      break;
    } else {
      protocol.Write("ERROR unknown command");
    }
  }

  peer.Detach();
  node.Stop();
  return 0;
}
