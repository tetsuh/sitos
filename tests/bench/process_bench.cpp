// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

// Process-isolated N08/N09 benchmark driver. The private line protocol is
// source-only benchmark plumbing; it is not a sitos wire or lifecycle API.
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sitos/param_cache.hpp"
#include "sitos/param_store.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#if defined(_WIN32)
int main(int argc, char**) {
  if (argc > 1)
    std::cout << "sitos_process_bench is Linux-only; use the hosted Ubuntu benchmark job\n";
  return 0;
}
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kScalarCount = 9999;
constexpr std::size_t kLutBytes = 100000000;
constexpr std::uint64_t kLutSeed = 0x9e3779b97f4a7c15ULL;
constexpr std::uint64_t kLutMultiplier = 0x2545f4914f6cdd1dULL;
constexpr std::string_view kLutSha256 =
    "7975a2b50c79617f9a7d0e02702cb2c0fa533dd083fc999e6316c852fc06f2aa";

std::string ReadLine(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 60000) <= 0) return {};
    const auto count = ::read(fd, &ch, 1);
    if (count == 0) return {};
    if (count < 0) {
      if (errno == EINTR) continue;
      return {};
    }
    if (ch == '\n') return line;
    line.push_back(ch);
  }
}

bool WriteLine(int fd, std::string_view line) {
  std::string message(line);
  message.push_back('\n');
  std::size_t offset = 0;
  while (offset < message.size()) {
    const auto count = ::write(fd, message.data() + offset, message.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::string Token() {
  std::array<unsigned char, 16> bytes{};
  std::random_device device;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(device());
  std::ostringstream out;
  for (const auto byte : bytes)
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
  return out.str();
}

std::int64_t Scalar(std::size_t index) {
  return static_cast<std::int64_t>(index * 104729ULL) - 500000000LL;
}

std::string N09Index(std::size_t index) {
  std::ostringstream value;
  value << std::setw(6) << std::setfill('0') << index;
  return value.str();
}

std::string N09Key(std::size_t index) { return "n09/v1/value/" + N09Index(index); }

std::string ThroughputSequence(std::size_t sequence) {
  std::ostringstream value;
  value << std::setw(10) << std::setfill('0') << sequence;
  return value.str();
}

std::string ThroughputKey(std::size_t trial, std::size_t producer, std::size_t sequence) {
  std::ostringstream key;
  key << "n09/v1/throughput/" << trial << '/' << producer << '/' << ThroughputSequence(sequence);
  return key.str();
}

std::int64_t ThroughputValue(std::size_t trial, std::size_t producer, std::size_t sequence) {
  return static_cast<std::int64_t>((trial << 48) | (producer << 40) | sequence);
}

std::string Sha256(std::span<const std::byte> input) {
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<unsigned char> data;
  data.reserve(input.size() + 72);
  for (auto byte : input) data.push_back(static_cast<unsigned char>(byte));
  const auto bits = static_cast<std::uint64_t>(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
    data.push_back(static_cast<unsigned char>(bits >> shift));
  for (std::size_t offset = 0; offset < data.size(); offset += 64) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<std::uint32_t>(data[offset + i * 4]) << 24) |
             (static_cast<std::uint32_t>(data[offset + i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(data[offset + i * 4 + 2]) << 8) | data[offset + i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], x = h[7];
    for (int i = 0; i < 64; ++i) {
      const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto t1 = x + s1 + ch + k[i] + w[i];
      const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto t2 = s0 + maj;
      x = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += x;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto value : h) out << std::setw(8) << value;
  return out.str();
}

std::vector<std::byte> Lut() {
  std::vector<std::byte> bytes;
  bytes.resize(kLutBytes);
  std::uint64_t state = kLutSeed;
  for (auto& byte : bytes) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const auto value = state * kLutMultiplier;
    byte = static_cast<std::byte>(value >> 56);
  }
  return bytes;
}

struct Child {
  pid_t pid = -1;
  int input = -1;
  int output = -1;
};

Child Spawn(const char* self, std::string_view role, std::string_view prefix,
            std::string_view storage, std::string_view config, int control_fd = -1,
            int control_peer_fd = -1, int node_control_fd = -1, int node_control_peer_fd = -1) {
  int parent_to_child[2]{};
  int child_to_parent[2]{};
  if (::pipe2(parent_to_child, O_CLOEXEC) != 0) throw std::runtime_error("pipe failed");
  if (::pipe2(child_to_parent, O_CLOEXEC) != 0) {
    ::close(parent_to_child[0]);
    ::close(parent_to_child[1]);
    throw std::runtime_error("pipe failed");
  }
  const auto pid = ::fork();
  if (pid < 0) {
    ::close(parent_to_child[0]);
    ::close(parent_to_child[1]);
    ::close(child_to_parent[0]);
    ::close(child_to_parent[1]);
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    ::close(parent_to_child[1]);
    ::close(child_to_parent[0]);
    if (fcntl(parent_to_child[0], F_SETFD, 0) != 0 || fcntl(child_to_parent[1], F_SETFD, 0) != 0)
      _exit(127);
    if (control_peer_fd >= 0) ::close(control_peer_fd);
    if (node_control_peer_fd >= 0) ::close(node_control_peer_fd);
    if (control_fd >= 0 && fcntl(control_fd, F_SETFD, 0) != 0) _exit(127);
    if (node_control_fd >= 0 && fcntl(node_control_fd, F_SETFD, 0) != 0) _exit(127);
    std::vector<std::string> arguments = {self,
                                          "--role",
                                          std::string(role),
                                          "--prefix",
                                          std::string(prefix),
                                          "--storage",
                                          std::string(storage),
                                          "--input-fd",
                                          std::to_string(parent_to_child[0]),
                                          "--output-fd",
                                          std::to_string(child_to_parent[1])};
    if (control_fd >= 0) {
      arguments.emplace_back("--control-fd");
      arguments.push_back(std::to_string(control_fd));
    }
    if (node_control_fd >= 0) {
      arguments.emplace_back("--node-control-fd");
      arguments.push_back(std::to_string(node_control_fd));
    }
    if (!config.empty()) {
      arguments.emplace_back("--config");
      arguments.emplace_back(config);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(self, argv.data());
    _exit(127);
  }
  ::close(parent_to_child[0]);
  ::close(child_to_parent[1]);
  return Child{pid, parent_to_child[1], child_to_parent[0]};
}

std::string ReadLineReady(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    const auto count = ::read(fd, &ch, 1);
    if (count <= 0) return {};
    if (ch == '\n') return line;
    line.push_back(ch);
  }
}

std::string ReadLineUntil(int fd, Clock::time_point deadline) {
  std::string line;
  char ch = '\0';
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) return {};
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, static_cast<int>(remaining.count())) <= 0) return {};
    const auto count = ::read(fd, &ch, 1);
    if (count == 0) return {};
    if (count < 0) {
      if (errno == EINTR) continue;
      return {};
    }
    if (ch == '\n') return line;
    line.push_back(ch);
  }
}

bool WaitFor(Child& child, std::string_view expected) {
  const auto line = ReadLine(child.output);
  return line == expected;
}

void Stop(Child& child) {
  if (child.input >= 0) {
    WriteLine(child.input, "STOP");
    ::close(child.input);
    child.input = -1;
  }
  if (child.output >= 0) {
    ::close(child.output);
    child.output = -1;
  }
  int status = 0;
  const auto graceful_deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < graceful_deadline) {
    const auto result = ::waitpid(child.pid, &status, WNOHANG);
    if (result == child.pid) {
      child.pid = -1;
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("benchmark child failed during shutdown");
      return;
    }
    if (result < 0 && errno == ECHILD) {
      child.pid = -1;
      return;
    }
    std::this_thread::yield();
  }
  ::kill(child.pid, SIGTERM);
  const auto term_deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < term_deadline) {
    const auto result = ::waitpid(child.pid, &status, WNOHANG);
    if (result == child.pid) {
      child.pid = -1;
      throw std::runtime_error("benchmark child required SIGTERM during shutdown");
    }
    if (result < 0 && errno == ECHILD) {
      child.pid = -1;
      return;
    }
    std::this_thread::yield();
  }
  ::kill(child.pid, SIGKILL);
  while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
  }
  child.pid = -1;
  throw std::runtime_error("benchmark child required SIGKILL during shutdown");
}

int RoleMain(std::string_view role, std::string_view prefix, std::string_view storage,
             std::string_view config, int input_fd, int output_fd, int control_fd,
             int node_control_fd) {
  auto transport_result = sitos::OpenZenohTransport(
      config.empty() ? std::nullopt : std::optional<std::string_view>(config));
  if (!transport_result.IsOk()) return 10;
  auto transport = std::shared_ptr<sitos::Transport>(std::move(transport_result).Value().release());
  sitos::ClientConfig client_config;
  client_config.prefix = std::string(prefix);
  // Keep each remote query below the frozen 60-second N08 sample deadline.
  client_config.query_timeout = std::chrono::seconds(55);
  if (!config.empty()) client_config.zenoh_config_json = std::string(config);

  constexpr std::string_view create_prefix = "CREATE ";
  constexpr std::string_view close_prefix = "CLOSE ";
  constexpr std::string_view throughput_prefix = "THROUGHPUT ";
  constexpr std::string_view control_prefix = "CONTROL ";
  constexpr std::string_view put_prefix = "PUT ";
  constexpr std::string_view put_ready_prefix = "PUT_READY ";
  constexpr std::string_view ping_prefix = "PING ";
  constexpr std::string_view observer_ready_prefix = "THROUGHPUT_OBSERVER_READY ";
  constexpr std::string_view throughput_ready_prefix = "THROUGHPUT_READY ";
  constexpr std::string_view attach_prefix = "ATTACH ";
  constexpr std::string_view verify_throughput_prefix = "VERIFY_THROUGHPUT ";
  constexpr std::string_view verify_n08_prefix = "VERIFY_N08 ";
  constexpr std::string_view verify_n09_prefix = "VERIFY_N09 ";
  constexpr std::string_view ack_prefix = "N08_VERIFY_ACK ";

  if (role == "node") {
    auto engine_result = sitos::RocksDBEngine::Open(std::string(storage));
    if (!engine_result.IsOk()) return 11;
    auto engine = std::move(engine_result).Value();
    auto* node_engine = engine.get();
    sitos::StorageNode node(*transport);
    sitos::StorageNodeConfig node_config;
    node_config.prefix = std::string(prefix);
    if (!node.Start(std::shared_ptr<sitos::StorageEngine>(std::move(engine)), node_config).IsOk())
      return 12;
    const auto expected_lut = Lut();
    std::mutex session_mutex;
    std::unordered_map<std::string, Clock::time_point> n08_starts;
    std::atomic<bool> node_ack_running{true};
    std::thread node_ack_thread;
    if (node_control_fd >= 0)
      node_ack_thread = std::thread([&] {
        while (node_ack_running.load(std::memory_order_acquire)) {
          const auto acknowledgement = ReadLine(node_control_fd);
          if (acknowledgement.empty()) break;
          if (acknowledgement.rfind(ack_prefix, 0) == 0) {
            const auto sid = acknowledgement.substr(ack_prefix.size());
            Clock::time_point started;
            {
              std::lock_guard lock(session_mutex);
              const auto found = n08_starts.find(sid);
              if (found == n08_starts.end()) continue;
              started = found->second;
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                    .count();
            WriteLine(output_fd, "N08_COMPLETE " + sid + " " + std::to_string(elapsed));
          }
        }
      });
    struct AckCleanup {
      sitos::StorageNode& node;
      std::atomic<bool>& running;
      std::thread& thread;
      int control_fd;
      ~AckCleanup() noexcept {
        running.store(false, std::memory_order_release);
        if (control_fd >= 0) ::shutdown(control_fd, SHUT_RDWR);
        node.Stop();
        if (thread.joinable()) thread.join();
      }
    } ack_cleanup{node, node_ack_running, node_ack_thread, node_control_fd};
    if (!WriteLine(output_fd, "READY node")) return 13;
    while (true) {
      const auto command = ReadLine(input_fd);
      if (command == "STOP" || command.empty()) break;
      if (command.rfind(create_prefix, 0) == 0) {
        const auto sid = command.substr(create_prefix.size());
        const auto started = Clock::now();
        const auto result = node.CreateSession(sid);
        {
          std::lock_guard lock(session_mutex);
          n08_starts[sid] = started;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
        if (!result.IsOk() ||
            !WriteLine(output_fd, "CREATE_ENTRY " + sid + " " +
                                      std::to_string(started.time_since_epoch().count())) ||
            !WriteLine(output_fd, "SESSION " + sid + " " + std::to_string(elapsed)))
          return 14;
      }
      if (command == "VERIFY_BASE") {
        std::size_t scalar_count = 0;
        std::vector<bool> seen_scalars(kScalarCount, false);
        bool valid =
            node_engine->List("n08/v1/scalar/", [&](std::string_view key, sitos::Bytes value) {
              const auto slash = key.rfind('/');
              if (slash == std::string_view::npos) return false;
              const auto suffix = std::string(key.substr(slash + 1));
              if (suffix.size() != 4) return false;
              std::size_t consumed = 0;
              std::size_t index = 0;
              try {
                index = static_cast<std::size_t>(std::stoull(suffix, &consumed));
              } catch (...) {
                return false;
              }
              if (consumed != suffix.size() || index >= kScalarCount ||
                  N09Index(index).substr(2) != suffix || seen_scalars[index])
                return false;
              seen_scalars[index] = true;
              const auto decoded = sitos::ParamValue::Decode(value);
              if (!decoded.has_value() || !decoded->As<std::int64_t>().has_value() ||
                  *decoded->As<std::int64_t>() != Scalar(index))
                return false;
              ++scalar_count;
              return true;
            });
        bool lut_valid = false;
        node_engine->Get("n08/v1/lut", [&](std::string_view, sitos::Bytes value) {
          const auto decoded = sitos::ParamValue::Decode(value);
          const auto body = decoded.has_value() ? decoded->AsSpan<std::byte>()
                                                : std::optional<std::span<const std::byte> >{};
          lut_valid =
              body.has_value() && body->size() == expected_lut.size() &&
              std::equal(body->begin(), body->end(), expected_lut.begin(), expected_lut.end()) &&
              Sha256(*body) == kLutSha256;
          return true;
        });
        if (valid && scalar_count == kScalarCount && lut_valid) {
          if (!WriteLine(output_fd, "BASE_READY")) return 17;
        } else {
          std::ostringstream status;
          status << "BASE_STATUS " << scalar_count << ' ' << (lut_valid ? 1 : 0) << ' '
                 << (valid ? 1 : 0);
          if (!WriteLine(output_fd, status.str())) return 18;
        }
      }
      if (command.rfind(close_prefix, 0) == 0) {
        const auto sid = command.substr(close_prefix.size());
        const auto result = node.CloseSession(sid);
        if (!result.IsOk()) return 15;
        if (!WriteLine(output_fd, "CLOSED " + sid)) return 16;
      }
    }
    return 0;
  }

  if (role == "writer") {
    auto store_result = sitos::ParamStore::Open(transport, client_config);
    if (!store_result.IsOk()) return 20;
    auto store = std::move(store_result).Value();
    struct DirectState {
      std::mutex mutex;
      std::condition_variable cv;
      std::optional<std::size_t> cache_ready_trial;
      std::size_t progress_observed = 0;
      std::string final_message;
      std::string error;
      bool running = true;
      bool reader_enabled = false;
    } direct;
    std::atomic<std::size_t> inflight{0};
    std::atomic<std::int64_t> release_ticks{0};
    std::thread direct_reader([&] {
      while (true) {
        {
          std::unique_lock lock(direct.mutex);
          direct.cv.wait(lock, [&] { return !direct.running || direct.reader_enabled; });
          if (!direct.running) break;
        }
        const auto message = ReadLine(control_fd);
        if (message.empty()) {
          std::lock_guard lock(direct.mutex);
          if (direct.running && direct.error.empty()) direct.error = "direct control EOF";
          direct.cv.notify_all();
          break;
        }
        std::lock_guard lock(direct.mutex);
        if (message.rfind("THROUGHPUT_CACHE_READY ", 0) == 0) {
          direct.cache_ready_trial = std::stoull(message.substr(23));
        } else if (message.rfind("THROUGHPUT_PROGRESS ", 0) == 0) {
          std::istringstream in(message.substr(20));
          std::size_t trial = 0, count = 0;
          in >> trial >> count;
          if (in && count >= direct.progress_observed) {
            const auto delta = count - direct.progress_observed;
            direct.progress_observed = count;
            auto available = inflight.load(std::memory_order_acquire);
            while (available != 0 && !inflight.compare_exchange_weak(
                                         available, available > delta ? available - delta : 0,
                                         std::memory_order_acq_rel)) {
            }
          }
        } else if (message.rfind("THROUGHPUT_DRAINED ", 0) == 0 ||
                   message.rfind("THROUGHPUT_DRAIN_ERROR ", 0) == 0) {
          direct.final_message = message;
          direct.reader_enabled = false;
        }
        direct.cv.notify_all();
      }
    });
    struct ReaderCleanup {
      DirectState& state;
      std::thread& thread;
      int fd;
      ~ReaderCleanup() noexcept {
        {
          std::lock_guard lock(state.mutex);
          state.running = false;
          state.reader_enabled = true;
        }
        state.cv.notify_all();
        ::shutdown(fd, SHUT_RDWR);
        if (thread.joinable()) thread.join();
      }
    } reader_cleanup{direct, direct_reader, control_fd};
    if (!WriteLine(output_fd, "READY writer")) return 21;
    constexpr std::size_t kThroughputInflightWindow = 128;
    while (true) {
      const auto command = ReadLine(input_fd);
      if (command == "STOP" || command.empty()) break;
      if (command == "POPULATE") {
        for (std::size_t i = 0; i < kScalarCount; ++i) {
          if (!store
                   .Put(
                       "base",
                       "n08/v1/scalar/" +
                           [&] {
                             std::ostringstream value;
                             value << std::setw(4) << std::setfill('0') << i;
                             return value.str();
                           }(),
                       Scalar(i), sitos::ParamStore::WriteOptions{.ack = false})
                   .IsOk())
            return 22;
        }
        auto lut = Lut();
        if (!store
                 .Put("base", "n08/v1/lut", sitos::ParamValue(std::move(lut)),
                      sitos::ParamStore::WriteOptions{.ack = false})
                 .IsOk())
          return 23;
        if (!WriteLine(output_fd, "POPULATED")) return 24;
      }
      if (command.rfind(control_prefix, 0) == 0) {
        const auto sequence = std::stoull(command.substr(control_prefix.size()));
        const auto padded = N09Index(sequence);
        const auto start = Clock::now();
        if (!WriteLine(control_fd, "PING " + padded)) return 29;
        const auto pong = ReadLine(control_fd);
        if (pong != "PONG " + padded) {
          WriteLine(output_fd, "CONTROL_ERROR " + pong);
          return 29;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (!WriteLine(output_fd, "CONTROL_SAMPLE " + padded + " " + std::to_string(elapsed)))
          return 29;
      }
      if (command.rfind(put_prefix, 0) == 0) {
        std::istringstream put_input(command.substr(put_prefix.size()));
        std::string sid;
        std::size_t index = 0;
        put_input >> sid >> index;
        const auto start = Clock::now();
        const auto result = store.Put("session/" + sid, N09Key(index),
                                      static_cast<std::int64_t>(0x13579bdf00000000ULL + index),
                                      sitos::ParamStore::WriteOptions{.ack = false});
        if (!result.IsOk()) {
          std::ostringstream diagnostic;
          diagnostic << "PUT_ERROR " << static_cast<int>(result.StatusCode()) << ' '
                     << result.Message();
          WriteLine(output_fd, diagnostic.str());
          return 25;
        }
        if (!WriteLine(control_fd, "PUT_READY " + N09Index(index))) return 26;
        const auto acknowledgement = ReadLine(control_fd);
        if (acknowledgement != "VERIFIED_N09 " + N09Index(index)) {
          WriteLine(output_fd, "PUT_ERROR_DIRECT " + acknowledgement);
          return 27;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (!WriteLine(output_fd, "PUT_SAMPLE " + N09Index(index) + " " + std::to_string(elapsed)))
          return 28;
      }
      if (command.rfind(throughput_prefix, 0) == 0) {
        std::istringstream in(command.substr(throughput_prefix.size()));
        std::string sid;
        std::size_t trial = 0, producers = 0;
        in >> sid >> trial >> producers;
        if (producers != 1 && producers != 4) return 29;
        std::vector<std::optional<sitos::ParamStore> > stores(producers);
        for (auto& slot : stores) {
          auto result = sitos::ParamStore::Open(transport, client_config);
          if (!result.IsOk()) {
            WriteLine(output_fd, "THROUGHPUT_OPEN_ERROR " + std::string(result.Message()));
            return 29;
          }
          slot.emplace(std::move(result).Value());
        }
        {
          std::lock_guard lock(direct.mutex);
          direct.cache_ready_trial.reset();
          direct.progress_observed = 0;
          direct.final_message.clear();
          direct.error.clear();
          direct.reader_enabled = true;
          direct.cv.notify_all();
        }
        if (!WriteLine(control_fd, "THROUGHPUT_OBSERVER_READY " + std::to_string(trial))) return 29;
        const auto setup_deadline = Clock::now() + std::chrono::seconds(60);
        {
          std::unique_lock lock(direct.mutex);
          if (!direct.cv.wait_until(lock, setup_deadline,
                                    [&] {
                                      return (direct.cache_ready_trial.has_value() &&
                                              *direct.cache_ready_trial == trial) ||
                                             !direct.error.empty();
                                    }) ||
              !direct.error.empty()) {
            WriteLine(output_fd, "THROUGHPUT_ERROR " + direct.error);
            return 29;
          }
        }
        std::atomic<bool> failed{false};
        std::atomic<std::size_t> ready_count{0};
        std::mutex ready_mutex;
        std::condition_variable ready_cv;
        std::barrier start_barrier(static_cast<std::ptrdiff_t>(producers + 1));
        std::vector<std::size_t> counts(producers);
        std::vector<std::thread> threads;
        for (std::size_t producer = 0; producer < producers; ++producer) {
          threads.emplace_back([&, producer] {
            ready_count.fetch_add(1, std::memory_order_release);
            ready_cv.notify_one();
            start_barrier.arrive_and_wait();
            const auto deadline = Clock::time_point(std::chrono::nanoseconds(
                                      release_ticks.load(std::memory_order_acquire))) +
                                  std::chrono::seconds(2);
            while (Clock::now() < deadline && !failed.load(std::memory_order_acquire)) {
              std::size_t available = inflight.load(std::memory_order_acquire);
              while (available < kThroughputInflightWindow &&
                     !inflight.compare_exchange_weak(available, available + 1,
                                                     std::memory_order_acq_rel)) {
              }
              if (available >= kThroughputInflightWindow) {
                if (Clock::now() >= deadline) break;
                std::this_thread::yield();
                continue;
              }
              const auto sequence = counts[producer];
              auto result =
                  stores[producer]->Put("session/" + sid, ThroughputKey(trial, producer, sequence),
                                        ThroughputValue(trial, producer, sequence),
                                        sitos::ParamStore::WriteOptions{.ack = false});
              if (!result.IsOk()) {
                inflight.fetch_sub(1, std::memory_order_release);
                failed.store(true, std::memory_order_release);
                return;
              }
              ++counts[producer];
            }
          });
        }
        {
          std::unique_lock lock(ready_mutex);
          if (!ready_cv.wait_until(lock, setup_deadline, [&] {
                return ready_count.load(std::memory_order_acquire) == producers;
              })) {
            failed.store(true, std::memory_order_release);
          }
        }
        if (failed.load(std::memory_order_acquire)) {
          for (auto& thread : threads) thread.join();
          WriteLine(output_fd, "THROUGHPUT_ERROR producer readiness timeout");
          return 29;
        }
        if (!WriteLine(output_fd, "THROUGHPUT_PRODUCER_READY " + std::to_string(trial) + " " +
                                      std::to_string(producers)))
          return 29;
        const auto release_command = ReadLineUntil(input_fd, setup_deadline);
        if (release_command != "THROUGHPUT_RELEASE " + std::to_string(trial)) {
          failed.store(true, std::memory_order_release);
          for (auto& thread : threads) thread.join();
          WriteLine(output_fd, "THROUGHPUT_ERROR release not received");
          return 29;
        }
        release_ticks.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
        start_barrier.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        if (failed.load(std::memory_order_acquire)) {
          WriteLine(output_fd, "THROUGHPUT_PUT_ERROR");
          return 29;
        }
        std::size_t total = 0;
        for (const auto count : counts) total += count;
        std::ostringstream ready;
        ready << "THROUGHPUT_READY " << trial << ' ' << producers << ' ' << total;
        for (const auto count : counts) ready << ' ' << count;
        ready << ' ' << release_ticks.load(std::memory_order_acquire);
        if (!WriteLine(control_fd, ready.str())) return 29;
        std::unique_lock lock(direct.mutex);
        if (!direct.cv.wait_until(
                lock,
                Clock::time_point(
                    std::chrono::nanoseconds(release_ticks.load(std::memory_order_acquire))) +
                    std::chrono::seconds(60),
                [&] { return !direct.final_message.empty() || !direct.error.empty(); })) {
          WriteLine(output_fd, "THROUGHPUT_ERROR drain deadline expired");
          return 29;
        }
        const auto final_message =
            direct.final_message.empty() ? direct.error : direct.final_message;
        if (final_message.rfind("THROUGHPUT_DRAINED ", 0) != 0) {
          WriteLine(output_fd, "THROUGHPUT_ERROR " + final_message);
          return 29;
        }
        std::istringstream drained_parser(final_message.substr(19));
        std::size_t drained_trial = 0, observed = 0;
        drained_parser >> drained_trial >> observed;
        if (drained_trial != trial || observed != total) {
          WriteLine(output_fd, "THROUGHPUT_ERROR " + final_message);
          return 29;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now() - Clock::time_point(std::chrono::nanoseconds(
                                                    release_ticks.load(std::memory_order_acquire))))
                                 .count();
        if (!WriteLine(output_fd, "THROUGHPUT_SAMPLE " + std::to_string(trial) + " " +
                                      std::to_string(total) + " " + std::to_string(elapsed)))
          return 29;
      }
    }
    return 0;
  }

  if (role == "cache") {
    auto cache_result = sitos::ParamCache::Open(transport, client_config);
    if (!cache_result.IsOk()) return 30;
    auto cache = std::move(cache_result).Value();
    auto observer_store_result = sitos::ParamStore::Open(transport, client_config);
    if (!observer_store_result.IsOk()) return 30;
    auto observer_store = std::move(observer_store_result).Value();
    std::mutex observer_mutex;
    std::mutex direct_write_mutex;
    std::condition_variable observer_cv;
    std::unordered_map<std::string, std::size_t> observed_keys;
    std::string observer_error;
    std::size_t active_trial = 0;
    std::size_t progress_sent = 0;
    std::optional<sitos::ParamSubscription> throughput_subscription;
    const auto write_direct = [&](std::string_view message) {
      std::lock_guard lock(direct_write_mutex);
      return WriteLine(control_fd, message);
    };
    const auto observer = [&](const sitos::ParamChange& change) {
      if (change.kind != sitos::ParamChangeKind::kPut || !change.value.has_value()) return;
      std::optional<std::size_t> progress;
      std::size_t progress_trial = 0;
      {
        std::lock_guard lock(observer_mutex);
        const auto marker = std::string("n09/v1/throughput/");
        if (change.key.rfind(marker, 0) != 0) return;
        std::istringstream in(change.key.substr(marker.size()));
        std::size_t trial = 0, producer = 0, sequence = 0;
        char slash1 = 0, slash2 = 0;
        if (!(in >> trial >> slash1 >> producer >> slash2 >> sequence) || slash1 != '/' ||
            slash2 != '/') {
          observer_error = "malformed throughput observation";
          observer_cv.notify_all();
          return;
        }
        const auto value = change.value->As<std::int64_t>();
        if (!value.has_value() || *value != ThroughputValue(trial, producer, sequence)) {
          observer_error = "mismatched throughput observation";
          observer_cv.notify_all();
          return;
        }
        const auto inserted = observed_keys.emplace(change.key, 1);
        if (!inserted.second) {
          observer_error = "duplicate throughput observation";
          observer_cv.notify_all();
          return;
        }
        if (trial == active_trial && observed_keys.size() - progress_sent >= 64) {
          progress_sent = observed_keys.size();
          progress_trial = trial;
          progress = progress_sent;
        }
        observer_cv.notify_all();
      }
      if (progress.has_value() &&
          !write_direct("THROUGHPUT_PROGRESS " + std::to_string(progress_trial) + " " +
                        std::to_string(*progress))) {
        std::lock_guard lock(observer_mutex);
        observer_error = "progress channel write failed";
        observer_cv.notify_all();
      }
    };
    const auto expected_lut = Lut();
    if (!WriteLine(output_fd, "READY cache")) return 31;
    while (true) {
      pollfd descriptors[2]{{input_fd, POLLIN, 0}, {control_fd, POLLIN, 0}};
      if (::poll(descriptors, 2, 60000) <= 0) break;
      const bool direct = descriptors[1].revents & POLLIN;
      const auto command = ReadLineReady(direct ? control_fd : input_fd);
      if (command.empty()) break;
      if (direct) {
        if (command.rfind(put_ready_prefix, 0) == 0) {
          const auto index = std::stoull(command.substr(put_ready_prefix.size()));
          const auto deadline = Clock::now() + std::chrono::seconds(10);
          while (true) {
            auto value = cache.Get<std::int64_t>(N09Key(index));
            if (value.IsOk()) {
              if (value.Value() != static_cast<std::int64_t>(0x13579bdf00000000ULL + index)) {
                WriteLine(control_fd, "N09_ERROR " + N09Index(index) + " value-mismatch");
                return 37;
              }
              if (!WriteLine(control_fd, "VERIFIED_N09 " + N09Index(index))) return 38;
              break;
            }
            if (Clock::now() >= deadline) {
              WriteLine(control_fd,
                        "N09_ERROR " + N09Index(index) + " " + std::string(value.Message()));
              return 37;
            }
            std::this_thread::yield();
          }
        } else if (command.rfind(ping_prefix, 0) == 0) {
          if (!WriteLine(control_fd,
                         "PONG " + N09Index(std::stoull(command.substr(ping_prefix.size())))))
            return 39;
        } else if (command.rfind(observer_ready_prefix, 0) == 0) {
          const auto trial = std::stoull(command.substr(observer_ready_prefix.size()));
          {
            std::lock_guard lock(observer_mutex);
            active_trial = trial;
            progress_sent = 0;
            observed_keys.clear();
            observer_error.clear();
          }
          if (!write_direct("THROUGHPUT_CACHE_READY " + std::to_string(trial))) return 39;
        } else if (command.rfind(throughput_ready_prefix, 0) == 0) {
          std::istringstream in(command.substr(throughput_ready_prefix.size()));
          std::size_t trial = 0, producers = 0, total = 0;
          std::int64_t release_ticks = 0;
          in >> trial >> producers >> total;
          std::vector<std::size_t> expected_counts(producers);
          for (auto& count : expected_counts) in >> count;
          in >> release_ticks;
          const auto drain_deadline =
              Clock::time_point(std::chrono::nanoseconds(release_ticks)) + std::chrono::seconds(60);
          std::size_t expected_total = 0;
          for (const auto count : expected_counts) expected_total += count;
          const auto throughput_error = [&](std::string reason) {
            write_direct("THROUGHPUT_DRAIN_ERROR " + std::to_string(trial) + " " + reason);
          };
          if (!in || producers == 0 || expected_total != total) {
            throughput_error("terminal-count-mismatch");
            return 39;
          }
          const auto trial_prefix = "n09/v1/throughput/" + std::to_string(trial) + "/";
          std::unordered_set<std::string> expected_keys;
          for (std::size_t producer = 0; producer < producers; ++producer)
            for (std::size_t sequence = 0; sequence < expected_counts[producer]; ++sequence)
              expected_keys.insert(ThroughputKey(trial, producer, sequence));
          std::size_t observed_for_trial = 0;
          {
            std::unique_lock lock(observer_mutex);
            const auto complete = [&] {
              observed_for_trial = 0;
              for (const auto& [key, count] : observed_keys)
                if (key.rfind(trial_prefix, 0) == 0 && count == 1) ++observed_for_trial;
              return observed_for_trial >= expected_total;
            };
            if (!observer_cv.wait_until(lock, drain_deadline,
                                        [&] { return !observer_error.empty() || complete(); })) {
              throughput_error("drain-timeout");
              return 39;
            }
            if (!observer_error.empty()) {
              throughput_error(observer_error);
              return 39;
            }
            if (!complete()) {
              throughput_error("missing-observation");
              return 39;
            }
          }
          for (std::size_t producer = 0; producer < producers; ++producer) {
            for (std::size_t sequence = 0; sequence < expected_counts[producer]; ++sequence) {
              while (true) {
                auto value = cache.Get<std::int64_t>(ThroughputKey(trial, producer, sequence));
                if (value.IsOk()) {
                  if (value.Value() != ThroughputValue(trial, producer, sequence)) {
                    throughput_error("value-mismatch");
                    return 39;
                  }
                  break;
                }
                if (Clock::now() >= drain_deadline) {
                  throughput_error("cache-get-timeout");
                  return 39;
                }
                std::this_thread::yield();
              }
            }
          }
          {
            std::lock_guard lock(observer_mutex);
            for (const auto& [key, count] : observed_keys) {
              if (key.rfind(trial_prefix, 0) != 0) continue;
              if (count != 1 || expected_keys.find(key) == expected_keys.end()) {
                throughput_error("extra-or-duplicate-observation");
                return 39;
              }
            }
            observed_for_trial = 0;
            for (const auto& [key, count] : observed_keys)
              if (key.rfind(trial_prefix, 0) == 0 && count == 1) ++observed_for_trial;
          }
          if (observed_for_trial != expected_total) {
            throughput_error("observed-count-mismatch");
            return 39;
          }
          if (!write_direct("THROUGHPUT_PROGRESS " + std::to_string(trial) + " " +
                            std::to_string(observed_for_trial)) ||
              !write_direct("THROUGHPUT_DRAINED " + std::to_string(trial) + " " +
                            std::to_string(observed_for_trial)))
            return 39;
        }
        continue;
      }
      if (command == "STOP") break;
      if (command == "DETACH") {
        if (throughput_subscription.has_value()) {
          throughput_subscription->Close();
          throughput_subscription.reset();
        }
        cache.Detach();
        {
          std::lock_guard lock(observer_mutex);
          observed_keys.clear();
          observer_error.clear();
        }
        if (!WriteLine(output_fd, "DETACHED")) return 32;
      }
      if (command.rfind(attach_prefix, 0) == 0) {
        const auto sid = command.substr(attach_prefix.size());
        const auto result = cache.Attach(sid);
        if (!result.IsOk()) {
          std::ostringstream diagnostic;
          diagnostic << "ATTACH_ERROR " << static_cast<int>(result.StatusCode()) << ' '
                     << result.Message();
          WriteLine(output_fd, diagnostic.str());
          return 33;
        }
        if (sid.find("-tp") != std::string::npos) {
          auto subscription =
              observer_store.Subscribe("session/" + sid, "n09/v1/throughput/", observer);
          if (!subscription.IsOk()) return 34;
          throughput_subscription.emplace(std::move(subscription).Value());
        }
        if (!WriteLine(output_fd, "ATTACHED " + sid)) return 34;
      }
      if (command.rfind(verify_n08_prefix, 0) == 0) {
        const auto n08_sid = command.substr(verify_n08_prefix.size());
        for (std::size_t i = 0; i < kScalarCount; ++i) {
          std::ostringstream key;
          key << "n08/v1/scalar/" << std::setw(4) << std::setfill('0') << i;
          auto value = cache.Get<std::int64_t>(key.str());
          if (!value.IsOk() || value.Value() != Scalar(i)) {
            std::cerr << "N08 scalar verification failed index=" << i << " key=" << key.str()
                      << " status=" << static_cast<int>(value.StatusCode())
                      << " message=" << value.Message() << '\n';
            return 34;
          }
        }
        auto lut = cache.GetSpan<std::byte>("n08/v1/lut");
        if (!lut.IsOk() || lut.Value().span.size() != kLutBytes) {
          std::cerr << "N08 LUT verification failed status=" << static_cast<int>(lut.StatusCode())
                    << " message=" << lut.Message() << '\n';
          return 35;
        }
        if (!std::equal(lut.Value().span.begin(), lut.Value().span.end(), expected_lut.begin(),
                        expected_lut.end()) ||
            Sha256(lut.Value().span) != kLutSha256)
          return 36;
        if (!WriteLine(node_control_fd, "N08_VERIFY_ACK " + n08_sid)) return 38;
        if (!WriteLine(output_fd, "VERIFIED_N08")) return 38;
      }
      if (command.rfind(verify_n09_prefix, 0) == 0) {
        std::size_t index = 0;
        std::istringstream(command.substr(verify_n09_prefix.size())) >> index;
        auto value = cache.Get<std::int64_t>(N09Key(index));
        if (!value.IsOk() ||
            value.Value() != static_cast<std::int64_t>(0x13579bdf00000000ULL + index)) {
          std::cerr << "N09 visibility failed index=" << index
                    << " status=" << static_cast<int>(value.StatusCode())
                    << " message=" << value.Message() << '\n';
          return 37;
        }
        if (!WriteLine(output_fd, "VERIFIED_N09 " + std::to_string(index))) return 38;
      }
      if (command.rfind(verify_throughput_prefix, 0) == 0) {
        std::istringstream in(command.substr(verify_throughput_prefix.size()));
        std::string sid;
        std::size_t trial = 0;
        std::size_t producers = 0;
        in >> sid >> trial >> producers;
        if (!in || producers == 0 || producers > 64) return 39;
        std::vector<std::size_t> counts(producers);
        std::size_t expected = 0;
        for (auto& count : counts) {
          in >> count;
          expected += count;
        }
        std::size_t observed = 0;
        for (std::size_t producer = 0; producer < producers; ++producer) {
          for (std::size_t sequence = 0; sequence < counts[producer]; ++sequence) {
            auto value = cache.Get<std::int64_t>(ThroughputKey(trial, producer, sequence));
            if (!value.IsOk() || value.Value() != ThroughputValue(trial, producer, sequence)) {
              std::cerr << "throughput drain mismatch trial=" << trial << " producer=" << producer
                        << " sequence=" << sequence << '\n';
              return 39;
            }
            ++observed;
          }
        }
        if (observed != expected ||
            !WriteLine(output_fd, "THROUGHPUT_DRAINED " + std::to_string(trial) + " " +
                                      std::to_string(observed)))
          return 39;
      }
      if (command.rfind(ping_prefix, 0) == 0) {
        const auto sequence = command.substr(ping_prefix.size());
        if (!WriteLine(output_fd, "PONG " + sequence)) return 39;
      }
    }
    cache.Detach();
    return 0;
  }
  return 40;
}

int CoordinatorMain(const char* self, const std::string& output, const std::string& config) {
  const auto token = Token();
  const auto executable = std::filesystem::absolute(self).string();
  auto prefix = "sitos/bench/n08/v1/" + token;
  auto storage = (std::filesystem::temp_directory_path() / ("sitos-bench-n08-" + token)).string();
  Child node{};
  Child writer{};
  Child cache{};
  int control[2]{-1, -1};
  int node_control[2]{-1, -1};
  try {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, node_control) != 0)
      throw std::runtime_error("N08 node/cache control socketpair failed");
    node = Spawn(executable.c_str(), "node", prefix, storage, config, -1, -1, node_control[0],
                 node_control[1]);
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0)
      throw std::runtime_error("writer/cache control socketpair failed");
    writer = Spawn(executable.c_str(), "writer", prefix, storage, config, control[0], control[1]);
    cache = Spawn(executable.c_str(), "cache", prefix, storage, config, control[1], control[0],
                  node_control[1], node_control[0]);
    ::close(control[0]);
    ::close(control[1]);
    ::close(node_control[0]);
    ::close(node_control[1]);
    control[0] = control[1] = node_control[0] = node_control[1] = -1;
    if (!WaitFor(node, "READY node") || !WaitFor(writer, "READY writer") ||
        !WaitFor(cache, "READY cache"))
      throw std::runtime_error("role readiness failed");
    WriteLine(writer.input, "POPULATE");
    if (!WaitFor(writer, "POPULATED")) throw std::runtime_error("population failed");
    const auto readiness_deadline = Clock::now() + std::chrono::seconds(60);
    while (true) {
      WriteLine(node.input, "VERIFY_BASE");
      const auto status = ReadLine(node.output);
      if (status == "BASE_READY") break;
      if (!status.starts_with("BASE_STATUS "))
        throw std::runtime_error("base readiness verification failed: " + status);
      if (Clock::now() >= readiness_deadline)
        throw std::runtime_error("base readiness deadline expired: " + status);
      WriteLine(writer.input, "POPULATE");
      if (!WaitFor(writer, "POPULATED"))
        throw std::runtime_error("base readiness resubmission failed");
    }
    std::vector<std::int64_t> n08;
    for (int sample = 0; sample < 6; ++sample) {
      const auto sid = "n08-v1-" + token + "-" + [&] {
        std::ostringstream s;
        s << std::setw(2) << std::setfill('0') << sample;
        return s.str();
      }();
      const auto admission_deadline = Clock::now() + std::chrono::seconds(60);
      WriteLine(node.input, "CREATE " + sid);
      const auto entry_response = ReadLineUntil(node.output, admission_deadline);
      std::istringstream entry_parser(entry_response);
      std::string entry_tag, entry_sid;
      std::int64_t entry_ticks = 0;
      entry_parser >> entry_tag >> entry_sid >> entry_ticks;
      if (entry_tag != "CREATE_ENTRY" || entry_sid != sid)
        throw std::runtime_error("session call-entry failed: " +
                                 (entry_response.empty() ? "EOF/timeout" : entry_response));
      const auto start = Clock::time_point(std::chrono::steady_clock::duration(entry_ticks));
      const auto sample_deadline = start + std::chrono::seconds(60);
      const auto session_response = ReadLineUntil(node.output, sample_deadline);
      if (session_response.rfind("SESSION ", 0) != 0)
        throw std::runtime_error("session create failed: " +
                                 (session_response.empty() ? "EOF/timeout" : session_response));
      WriteLine(cache.input, "ATTACH " + sid);
      const auto attach_response = ReadLineUntil(cache.output, sample_deadline);
      if (attach_response != "ATTACHED " + sid)
        throw std::runtime_error("cache attach failed: " +
                                 (attach_response.empty() ? "EOF/timeout" : attach_response));
      WriteLine(cache.input, "VERIFY_N08 " + sid);
      const auto verify_response = ReadLineUntil(cache.output, sample_deadline);
      if (verify_response != "VERIFIED_N08")
        throw std::runtime_error("N08 verification failed: " +
                                 (verify_response.empty() ? "EOF/timeout" : verify_response));
      const auto complete_response = ReadLineUntil(node.output, sample_deadline);
      std::istringstream complete_parser(complete_response);
      std::string complete_tag, complete_sid;
      std::int64_t elapsed = 0;
      complete_parser >> complete_tag >> complete_sid >> elapsed;
      if (complete_tag != "N08_COMPLETE" || complete_sid != sid || elapsed <= 0)
        throw std::runtime_error("N08 timer completion failed: " + complete_response);
      n08.push_back(elapsed);
      const auto cleanup_deadline = Clock::now() + std::chrono::seconds(5);
      WriteLine(cache.input, "DETACH");
      const auto detach_response = ReadLineUntil(cache.output, cleanup_deadline);
      if (detach_response != "DETACHED")
        throw std::runtime_error("N08 detach failed: " +
                                 (detach_response.empty() ? "EOF/timeout" : detach_response));
      WriteLine(node.input, "CLOSE " + sid);
      const auto close_response = ReadLineUntil(node.output, cleanup_deadline);
      if (close_response != "CLOSED " + sid)
        throw std::runtime_error("N08 close failed: " +
                                 (close_response.empty() ? "EOF/timeout" : close_response));
    }
    Stop(cache);
    Stop(writer);
    Stop(node);
    std::error_code n08_cleanup_error;
    std::filesystem::remove_all(storage, n08_cleanup_error);
    if (n08_cleanup_error) throw std::runtime_error("N08 storage cleanup failed");
    prefix = "sitos/bench/n09/v1/" + token;
    storage = (std::filesystem::temp_directory_path() / ("sitos-bench-n09-" + token)).string();
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0)
      throw std::runtime_error("N09 writer/cache control socketpair failed");
    node = Spawn(executable.c_str(), "node", prefix, storage, config);
    writer = Spawn(executable.c_str(), "writer", prefix, storage, config, control[0], control[1]);
    cache = Spawn(executable.c_str(), "cache", prefix, storage, config, control[1], control[0]);
    ::close(control[0]);
    control[0] = -1;
    ::close(control[1]);
    control[1] = -1;
    if (!WaitFor(node, "READY node") || !WaitFor(writer, "READY writer") ||
        !WaitFor(cache, "READY cache"))
      throw std::runtime_error("N09 role readiness failed");
    const auto n09_sid = "n09-v1-" + token;
    WriteLine(node.input, "CREATE " + n09_sid);
    if (ReadLine(node.output).rfind("CREATE_ENTRY " + n09_sid + " ", 0) != 0)
      throw std::runtime_error("N09 session call-entry failed");
    if (ReadLine(node.output).rfind("SESSION ", 0) != 0)
      throw std::runtime_error("N09 session create failed");
    WriteLine(cache.input, "DETACH");
    if (!WaitFor(cache, "DETACHED")) throw std::runtime_error("N09 cache detach failed");
    WriteLine(cache.input, "ATTACH " + n09_sid);
    if (!WaitFor(cache, "ATTACHED " + n09_sid)) throw std::runtime_error("N09 cache attach failed");
    std::vector<std::vector<std::int64_t> > n09(5);
    auto run_visibility = [&](std::size_t repetition, std::size_t sample,
                              Clock::time_point repetition_deadline) {
      const auto index = repetition == 0 ? sample : 20 + (repetition - 1) * 200 + sample;
      const auto deadline = std::min(repetition_deadline, Clock::now() + std::chrono::seconds(10));
      WriteLine(writer.input, "PUT " + n09_sid + " " + std::to_string(index));
      const auto sample_response = ReadLineUntil(writer.output, deadline);
      const auto prefix = "PUT_SAMPLE " + N09Index(index) + " ";
      if (sample_response.rfind(prefix, 0) != 0)
        throw std::runtime_error("N09 timing acknowledgement failed: " +
                                 (sample_response.empty() ? "EOF/timeout" : sample_response));
      if (repetition > 0)
        n09[repetition - 1].push_back(std::stoll(sample_response.substr(prefix.size())));
    };
    const auto warmup_deadline = Clock::now() + std::chrono::seconds(60);
    for (std::size_t sample = 0; sample < 20; ++sample) run_visibility(0, sample, warmup_deadline);
    for (std::size_t repetition = 1; repetition <= 5; ++repetition) {
      const auto repetition_deadline = Clock::now() + std::chrono::seconds(60);
      for (std::size_t sample = 0; sample < 200; ++sample)
        run_visibility(repetition, sample, repetition_deadline);
    }
    std::vector<std::vector<std::int64_t> > n09_control(5);
    auto run_control = [&](std::size_t repetition, std::size_t sample,
                           Clock::time_point repetition_deadline) {
      const auto deadline = std::min(repetition_deadline, Clock::now() + std::chrono::seconds(10));
      const auto index = repetition == 0 ? sample : 20 + (repetition - 1) * 200 + sample;
      const auto sequence = N09Index(index);
      WriteLine(writer.input, "CONTROL " + sequence);
      const auto response = ReadLineUntil(writer.output, deadline);
      const auto prefix = "CONTROL_SAMPLE " + sequence + " ";
      if (response.rfind(prefix, 0) != 0)
        throw std::runtime_error("N09 control timing failed: " + response);
      if (repetition > 0)
        n09_control[repetition - 1].push_back(std::stoll(response.substr(prefix.size())));
    };
    const auto control_warmup_deadline = Clock::now() + std::chrono::seconds(60);
    for (std::size_t sample = 0; sample < 20; ++sample)
      run_control(0, sample, control_warmup_deadline);
    for (std::size_t repetition = 1; repetition <= 5; ++repetition) {
      const auto repetition_deadline = Clock::now() + std::chrono::seconds(60);
      for (std::size_t sample = 0; sample < 200; ++sample)
        run_control(repetition, sample, repetition_deadline);
    }
    struct ThroughputSample {
      std::size_t count;
      std::int64_t elapsed_ns;
    };
    std::vector<std::vector<ThroughputSample> > throughput_1p(5), throughput_4p(5);
    auto run_throughput = [&](std::size_t producers, std::size_t trial,
                              std::string_view throughput_sid) {
      const auto setup_deadline = Clock::now() + std::chrono::seconds(60);
      WriteLine(writer.input, "THROUGHPUT " + std::string(throughput_sid) + " " +
                                  std::to_string(trial) + " " + std::to_string(producers));
      const auto ready = ReadLineUntil(writer.output, setup_deadline);
      const auto ready_prefix = "THROUGHPUT_PRODUCER_READY " + std::to_string(trial) + " ";
      if (ready.rfind(ready_prefix, 0) != 0)
        throw std::runtime_error("throughput producer readiness failed: " + ready);
      if (!WriteLine(writer.input, "THROUGHPUT_RELEASE " + std::to_string(trial)))
        throw std::runtime_error("throughput release failed");
      const auto sample_deadline = Clock::now() + std::chrono::seconds(60);
      const auto sample = ReadLineUntil(writer.output, sample_deadline);
      if (sample.rfind("THROUGHPUT_SAMPLE ", 0) != 0)
        throw std::runtime_error("throughput drain/timing failed: " + sample);
      std::istringstream parsed(sample.substr(18));
      std::size_t parsed_trial = 0, total = 0;
      std::int64_t elapsed_ns = 0;
      parsed >> parsed_trial >> total >> elapsed_ns;
      if (parsed_trial != trial || total == 0 || elapsed_ns <= 0)
        throw std::runtime_error("throughput count/duration evidence invalid: " + sample);
      if (trial > 0)
        (producers == 1 ? throughput_1p : throughput_4p)[trial - 1].push_back(
            ThroughputSample{total, elapsed_ns});
    };
    auto run_throughput_group = [&](std::size_t producers) {
      const auto throughput_sid = n09_sid + "-tp" + std::to_string(producers);
      WriteLine(node.input, "CREATE " + throughput_sid);
      if (ReadLine(node.output).rfind("CREATE_ENTRY " + throughput_sid + " ", 0) != 0 ||
          ReadLine(node.output).rfind("SESSION ", 0) != 0)
        throw std::runtime_error("throughput session create failed");
      WriteLine(cache.input, "DETACH");
      if (!WaitFor(cache, "DETACHED")) throw std::runtime_error("throughput detach failed");
      WriteLine(cache.input, "ATTACH " + throughput_sid);
      if (!WaitFor(cache, "ATTACHED " + throughput_sid))
        throw std::runtime_error("throughput attach failed");
      for (std::size_t trial = 0; trial <= 5; ++trial)
        run_throughput(producers, trial, throughput_sid);
      WriteLine(cache.input, "DETACH");
      if (!WaitFor(cache, "DETACHED")) throw std::runtime_error("throughput final detach failed");
      WriteLine(node.input, "CLOSE " + throughput_sid);
      if (ReadLineUntil(node.output, Clock::now() + std::chrono::seconds(5)) !=
          "CLOSED " + throughput_sid)
        throw std::runtime_error("throughput close failed");
    };
    run_throughput_group(1);
    run_throughput_group(4);
    WriteLine(node.input, "CLOSE " + n09_sid);
    const auto n09_close = ReadLineUntil(node.output, Clock::now() + std::chrono::seconds(5));
    if (n09_close != "CLOSED " + n09_sid)
      throw std::runtime_error("N09 close failed: " +
                               (n09_close.empty() ? "EOF/timeout" : n09_close));
    std::ofstream raw(output);
    raw << "{\n  \"schema_version\": \"benchmark-v1\",\n  \"scenario_execution\": {\"N08\": "
           "\"completed\", \"N09\": \"completed\", \"N09_CONTROL_RTT_V1\": \"completed\", "
           "\"N09_CALLBACK_THROUGHPUT_S64_1P_V1\": \"completed\", "
           "\"N09_CALLBACK_THROUGHPUT_S64_4P_V1\": \"completed\"},\n  "
           "\"n08_samples_ns\": [";
    for (std::size_t i = 1; i < n08.size(); ++i) raw << (i == 1 ? "" : ",") << n08[i];
    raw << "],\n  \"n09_visibility_samples_ns\": [";
    for (const auto& group : n09) {
      raw << "[";
      for (std::size_t i = 0; i < group.size(); ++i) raw << (i ? "," : "") << group[i];
      raw << "],";
    }
    raw.seekp(-1, std::ios_base::cur);
    raw << "],\n  \"n09_control_rtt_samples_ns\": [";
    for (const auto& group : n09_control) {
      raw << "[";
      for (std::size_t i = 0; i < group.size(); ++i) raw << (i ? "," : "") << group[i];
      raw << "],";
    }
    raw.seekp(-1, std::ios_base::cur);
    raw << "],\n  \"N09_CALLBACK_THROUGHPUT_S64_1P_V1_counts\": [";
    bool first = true;
    for (const auto& group : throughput_1p)
      for (const auto sample : group) {
        raw << (first ? "" : ",") << sample.count;
        first = false;
      }
    raw << "],\n  \"N09_CALLBACK_THROUGHPUT_S64_1P_V1_elapsed_ns\": [";
    first = true;
    for (const auto& group : throughput_1p)
      for (const auto sample : group) {
        raw << (first ? "" : ",") << sample.elapsed_ns;
        first = false;
      }
    raw << "],\n  \"N09_CALLBACK_THROUGHPUT_S64_4P_V1_counts\": [";
    first = true;
    for (const auto& group : throughput_4p)
      for (const auto sample : group) {
        raw << (first ? "" : ",") << sample.count;
        first = false;
      }
    raw << "],\n  \"N09_CALLBACK_THROUGHPUT_S64_4P_V1_elapsed_ns\": [";
    first = true;
    for (const auto& group : throughput_4p)
      for (const auto sample : group) {
        raw << (first ? "" : ",") << sample.elapsed_ns;
        first = false;
      }
    raw << "]\n}\n";
    Stop(cache);
    Stop(writer);
    Stop(node);
    std::error_code error;
    std::filesystem::remove_all(storage, error);
    return error ? 41 : 0;
  } catch (const std::exception& error) {
    std::cerr << "process benchmark failure: " << error.what() << '\n';
    if (control[0] >= 0) ::close(control[0]);
    if (control[1] >= 0) ::close(control[1]);
    if (node_control[0] >= 0) ::close(node_control[0]);
    if (node_control[1] >= 0) ::close(node_control[1]);
    for (const auto pid : {node.pid, writer.pid, cache.pid})
      if (pid > 0) ::kill(pid, SIGTERM);
    const auto reap = [](pid_t pid) {
      if (pid <= 0) return;
      int status = 0;
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (Clock::now() < deadline) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return;
        std::this_thread::yield();
      }
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    };
    reap(node.pid);
    reap(writer.pid);
    reap(cache.pid);
    node.pid = writer.pid = cache.pid = -1;
    for (auto* child : {&node, &writer, &cache}) {
      if (child->input >= 0) ::close(child->input);
      if (child->output >= 0) ::close(child->output);
      child->input = child->output = -1;
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(storage, cleanup_error);
    if (cleanup_error) std::cerr << "storage cleanup failed: " << cleanup_error.message() << '\n';
    return 42;
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string role;
  std::string prefix;
  std::string storage;
  std::string config;
  std::string output = "process-bench.json";
  int input_fd = -1;
  int output_fd = -1;
  int control_fd = -1;
  int node_control_fd = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--role" && i + 1 < argc)
      role = argv[++i];
    else if (arg == "--prefix" && i + 1 < argc)
      prefix = argv[++i];
    else if (arg == "--storage" && i + 1 < argc)
      storage = argv[++i];
    else if (arg == "--config" && i + 1 < argc)
      config = argv[++i];
    else if (arg == "--input-fd" && i + 1 < argc)
      input_fd = std::stoi(argv[++i]);
    else if (arg == "--output-fd" && i + 1 < argc)
      output_fd = std::stoi(argv[++i]);
    else if (arg == "--control-fd" && i + 1 < argc)
      control_fd = std::stoi(argv[++i]);
    else if (arg == "--node-control-fd" && i + 1 < argc)
      node_control_fd = std::stoi(argv[++i]);
    else if (arg == "--output" && i + 1 < argc)
      output = argv[++i];
    else if (arg == "--help") {
      std::cout << "sitos_process_bench --output <raw-json>\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return 2;
    }
  }
  if (!role.empty())
    return RoleMain(role, prefix, storage, config, input_fd, output_fd, control_fd,
                    node_control_fd);
  ::signal(SIGPIPE, SIG_IGN);
  return CoordinatorMain(argv[0], output, config);
}
#endif
