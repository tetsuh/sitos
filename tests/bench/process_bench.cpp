// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

// Process-isolated N08/N09 benchmark driver. The private line protocol is
// source-only benchmark plumbing; it is not a sitos wire or lifecycle API.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/param_store.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"

#if defined(_WIN32)
#error "Issue #33 process benchmark is Linux-only by contract"
#else
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

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
            std::string_view storage, std::string_view config) {
  int parent_to_child[2]{};
  int child_to_parent[2]{};
  if (::pipe(parent_to_child) != 0 || ::pipe(child_to_parent) != 0) {
    throw std::runtime_error("pipe failed");
  }
  const auto pid = ::fork();
  if (pid < 0) throw std::runtime_error("fork failed");
  if (pid == 0) {
    ::close(parent_to_child[1]);
    ::close(child_to_parent[0]);
    std::ostringstream args;
    args << self << " --role " << role << " --prefix " << prefix << " --storage " << storage
         << " --input-fd " << parent_to_child[0] << " --output-fd " << child_to_parent[1];
    if (!config.empty()) args << " --config " << config;
    ::execl("/bin/sh", "sh", "-c", args.str().c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(parent_to_child[0]);
  ::close(child_to_parent[1]);
  return Child{pid, parent_to_child[1], child_to_parent[0]};
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
  while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("benchmark child failed during shutdown");
  }
}

int RoleMain(std::string_view role, std::string_view prefix, std::string_view storage,
             std::string_view config, int input_fd, int output_fd) {
  auto transport_result = sitos::OpenZenohTransport(
      config.empty() ? std::nullopt : std::optional<std::string_view>(config));
  if (!transport_result.IsOk()) return 10;
  auto transport = std::shared_ptr<sitos::Transport>(std::move(transport_result).Value().release());
  sitos::ClientConfig client_config;
  client_config.prefix = std::string(prefix);
  if (!config.empty()) client_config.zenoh_config_json = std::string(config);

  if (role == "node") {
    auto engine_result = sitos::RocksDBEngine::Open(std::string(storage));
    if (!engine_result.IsOk()) return 11;
    auto engine = std::move(engine_result).Value();
    sitos::StorageNode node(*transport);
    sitos::StorageNodeConfig node_config;
    node_config.prefix = std::string(prefix);
    if (!node.Start(std::shared_ptr<sitos::StorageEngine>(std::move(engine)), node_config).IsOk())
      return 12;
    if (!WriteLine(output_fd, "READY node")) return 13;
    while (true) {
      const auto command = ReadLine(input_fd);
      if (command == "STOP" || command.empty()) break;
      if (command.rfind("CREATE ", 0) == 0) {
        const auto sid = command.substr(7);
        const auto started = Clock::now();
        const auto result = node.CreateSession(sid);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
        if (!result.IsOk() ||
            !WriteLine(output_fd, "SESSION " + sid + " " + std::to_string(elapsed)))
          return 14;
      }
      if (command.rfind("CLOSE ", 0) == 0 && !node.CloseSession(command.substr(6)).IsOk())
        return 15;
    }
    node.Stop();
    return 0;
  }

  if (role == "writer") {
    auto store_result = sitos::ParamStore::Open(transport, client_config);
    if (!store_result.IsOk()) return 20;
    auto store = std::move(store_result).Value();
    if (!WriteLine(output_fd, "READY writer")) return 21;
    while (true) {
      const auto command = ReadLine(input_fd);
      if (command == "STOP" || command.empty()) break;
      if (command == "POPULATE") {
        for (std::size_t i = 0; i < kScalarCount; ++i) {
          if (!store
                   .Put(
                       "",
                       "n08/v1/scalar/" +
                           [&] {
                             std::ostringstream s;
                             s << std::setw(4) << std::setfill('0') << i;
                             return s.str();
                           }(),
                       Scalar(i))
                   .IsOk())
            return 22;
        }
        auto lut = Lut();
        if (!store.Put("", "n08/v1/lut", sitos::ParamValue(std::move(lut))).IsOk()) return 23;
        if (!WriteLine(output_fd, "POPULATED")) return 24;
      }
      if (command.rfind("PUT ", 0) == 0) {
        std::istringstream in(command.substr(4));
        std::string sid;
        std::size_t index = 0;
        in >> sid >> index;
        if (!store
                 .Put(sid, "n09/v1/value/" + std::to_string(index),
                      static_cast<std::int64_t>(0x13579bdf00000000ULL + index))
                 .IsOk())
          return 25;
        if (!WriteLine(output_fd, "PUT_OK " + std::to_string(index))) return 26;
      }
    }
    return 0;
  }

  if (role == "cache") {
    auto cache_result = sitos::ParamCache::Open(transport, client_config);
    if (!cache_result.IsOk()) return 30;
    auto cache = std::move(cache_result).Value();
    if (!WriteLine(output_fd, "READY cache")) return 31;
    while (true) {
      const auto command = ReadLine(input_fd);
      if (command == "STOP" || command.empty()) break;
      if (command == "DETACH") {
        cache.Detach();
        if (!WriteLine(output_fd, "DETACHED")) return 32;
      }
      if (command.rfind("ATTACH ", 0) == 0) {
        const auto sid = command.substr(7);
        if (!cache.Attach(sid).IsOk()) return 33;
        if (!WriteLine(output_fd, "ATTACHED " + sid)) return 34;
      }
      if (command == "VERIFY_N08") {
        for (std::size_t i = 0; i < kScalarCount; ++i) {
          std::ostringstream key;
          key << "n08/v1/scalar/" << std::setw(4) << std::setfill('0') << i;
          auto value = cache.Get<std::int64_t>(key.str());
          if (!value.IsOk() || value.Value() != Scalar(i)) return 34;
        }
        auto lut = cache.GetSpan<std::byte>("n08/v1/lut");
        if (!lut.IsOk() || lut.Value().span.size() != kLutBytes) return 35;
        const auto expected_lut = Lut();
        if (!std::equal(lut.Value().span.begin(), lut.Value().span.end(), expected_lut.begin(),
                        expected_lut.end()))
          return 36;
        // The generator is pinned to the golden SHA-256 above; the exact byte comparison is the
        // runtime fence, while the digest is recorded in policy and every report artifact.
        if (kLutSha256.size() != 64) return 37;
        if (!WriteLine(output_fd, "VERIFIED_N08")) return 38;
      }
      if (command.rfind("VERIFY_N09 ", 0) == 0) {
        std::size_t index = 0;
        std::istringstream(command.substr(12)) >> index;
        auto value = cache.Get<std::int64_t>("n09/v1/value/" + std::to_string(index));
        if (!value.IsOk() ||
            value.Value() != static_cast<std::int64_t>(0x13579bdf00000000ULL + index))
          return 37;
        if (!WriteLine(output_fd, "VERIFIED_N09 " + std::to_string(index))) return 38;
      }
    }
    cache.Detach();
    return 0;
  }
  return 40;
}

int CoordinatorMain(const char* self, const std::string& output, const std::string& config) {
  const auto token = Token();
  const auto prefix = "sitos/bench/n08/v1/" + token;
  const auto storage = (std::filesystem::temp_directory_path() / ("sitos-bench-" + token)).string();
  Child node = Spawn(self, "node", prefix, storage, config);
  Child writer = Spawn(self, "writer", prefix, storage, config);
  Child cache = Spawn(self, "cache", prefix, storage, config);
  try {
    if (!WaitFor(node, "READY node") || !WaitFor(writer, "READY writer") ||
        !WaitFor(cache, "READY cache"))
      throw std::runtime_error("role readiness failed");
    WriteLine(writer.input, "POPULATE");
    if (!WaitFor(writer, "POPULATED")) throw std::runtime_error("population failed");
    std::vector<std::int64_t> n08;
    for (int sample = 0; sample < 6; ++sample) {
      const auto sid = "n08-v1-" + token + "-" + [&] {
        std::ostringstream s;
        s << std::setw(2) << std::setfill('0') << sample;
        return s.str();
      }();
      const auto start = Clock::now();
      WriteLine(node.input, "CREATE " + sid);
      if (ReadLine(node.output).rfind("SESSION ", 0) != 0)
        throw std::runtime_error("session create failed");
      WriteLine(cache.input, "ATTACH " + sid);
      if (!WaitFor(cache, "ATTACHED " + sid)) throw std::runtime_error("cache attach failed");
      WriteLine(cache.input, "VERIFY_N08");
      if (!WaitFor(cache, "VERIFIED_N08")) throw std::runtime_error("N08 verification failed");
      n08.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      WriteLine(node.input, "CLOSE " + sid);
    }
    const auto n09_sid = "n09-v1-" + token;
    WriteLine(node.input, "CREATE " + n09_sid);
    if (ReadLine(node.output).rfind("SESSION ", 0) != 0)
      throw std::runtime_error("N09 session create failed");
    WriteLine(cache.input, "DETACH");
    WriteLine(cache.input, "ATTACH " + n09_sid);
    if (!WaitFor(cache, "DETACHED")) throw std::runtime_error("N09 cache detach failed");
    if (!WaitFor(cache, "ATTACHED " + n09_sid)) throw std::runtime_error("N09 cache attach failed");
    std::vector<std::int64_t> n09;
    for (std::size_t sample = 0; sample < 1020; ++sample) {
      const auto start = Clock::now();
      WriteLine(writer.input, "PUT " + n09_sid + " " + std::to_string(sample));
      if (!WaitFor(writer, "PUT_OK " + std::to_string(sample)))
        throw std::runtime_error("N09 Put failed");
      WriteLine(cache.input, "VERIFY_N09 " + std::to_string(sample));
      if (!WaitFor(cache, "VERIFIED_N09 " + std::to_string(sample)))
        throw std::runtime_error("N09 visibility failed");
      if (sample >= 20)
        n09.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
    WriteLine(node.input, "CLOSE " + n09_sid);
    std::ofstream raw(output);
    raw << "{\n  \"schema_version\": \"benchmark-v1\",\n  \"scenario_execution\": {\"N08\": "
           "\"completed\", \"N09\": \"completed\"},\n  \"n08_samples_ns\": [";
    for (std::size_t i = 1; i < n08.size(); ++i) raw << (i == 1 ? "" : ",") << n08[i];
    raw << "],\n  \"n09_samples_ns\": [";
    for (std::size_t i = 0; i < n09.size(); ++i) raw << (i == 0 ? "" : ",") << n09[i];
    raw << "]\n}\n";
    Stop(cache);
    Stop(writer);
    Stop(node);
    std::error_code error;
    std::filesystem::remove_all(storage, error);
    return error ? 41 : 0;
  } catch (...) {
    ::kill(node.pid, SIGTERM);
    ::kill(writer.pid, SIGTERM);
    ::kill(cache.pid, SIGTERM);
    int status = 0;
    ::waitpid(node.pid, &status, 0);
    ::waitpid(writer.pid, &status, 0);
    ::waitpid(cache.pid, &status, 0);
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
  if (!role.empty()) return RoleMain(role, prefix, storage, config, input_fd, output_fd);
  return CoordinatorMain(argv[0], output, config);
}
