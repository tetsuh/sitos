// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <sitos/sitos.hpp>
#ifdef SITOS_WITH_ROCKSDB
#include <sitos/rocksdb_engine.hpp>
#endif

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

namespace {

struct Options {
  std::string engine = "inmemory";
  std::string rocksdb_path;
  std::string prefix = "sitos";
  std::optional<std::string> zenoh_config;
  std::optional<std::string> zenoh_config_file;
  bool engine_seen = false;
  bool rocksdb_path_seen = false;
  bool prefix_seen = false;
};

void PrintHelp() {
  std::cout << "Usage: sitobolon --help\n"
            << "       sitobolon [--engine inmemory|rocksdb] [--rocksdb-path <path>]\n"
            << "                [--prefix <prefix>]\n"
            << "                [--zenoh-config <complete-json5> |\n"
            << "                 --zenoh-config-file <path>]\n\n"
            << "Options:\n"
            << "  --engine <inmemory|rocksdb>  Storage engine (default: inmemory).\n"
            << "  --rocksdb-path <path>        RocksDB path; required for rocksdb.\n"
            << "  --prefix <prefix>            Valid sitos key prefix (default: sitos).\n"
            << "  --zenoh-config <complete-json5>\n"
            << "                               Complete Zenoh JSON5 configuration.\n"
            << "  --zenoh-config-file <path>   Read a complete Zenoh JSON5 configuration.\n"
            << "  --help                       Show this help (must be the only argument).\n";
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help") {
      error = "--help must be the sole argument";
      return false;
    }
    auto value = [&](std::string_view name) -> std::optional<std::string> {
      if (index + 1 >= argc || std::string_view(argv[index + 1]).starts_with("--")) {
        error = std::string(name) + " requires a value";
        return std::nullopt;
      }
      ++index;
      return std::string(argv[index]);
    };
    if (argument == "--engine") {
      auto value_text = value(argument);
      if (!value_text) return false;
      if (options.engine_seen) {
        error = "duplicate --engine";
        return false;
      }
      options.engine_seen = true;
      options.engine = std::move(*value_text);
    } else if (argument == "--rocksdb-path") {
      auto value_text = value(argument);
      if (!value_text) return false;
      if (options.rocksdb_path_seen) {
        error = "duplicate --rocksdb-path";
        return false;
      }
      options.rocksdb_path_seen = true;
      options.rocksdb_path = std::move(*value_text);
    } else if (argument == "--prefix") {
      auto value_text = value(argument);
      if (!value_text) return false;
      if (options.prefix_seen) {
        error = "duplicate --prefix";
        return false;
      }
      options.prefix_seen = true;
      options.prefix = std::move(*value_text);
    } else if (argument == "--zenoh-config") {
      auto value_text = value(argument);
      if (!value_text) return false;
      if (options.zenoh_config.has_value() || options.zenoh_config_file.has_value()) {
        error = "Zenoh configuration options are mutually exclusive or duplicated";
        return false;
      }
      options.zenoh_config = std::move(*value_text);
    } else if (argument == "--zenoh-config-file") {
      auto value_text = value(argument);
      if (!value_text) return false;
      if (options.zenoh_config.has_value() || options.zenoh_config_file.has_value()) {
        error = "Zenoh configuration options are mutually exclusive or duplicated";
        return false;
      }
      options.zenoh_config_file = std::move(*value_text);
    } else {
      error = "unknown option: " + std::string(argument);
      return false;
    }
  }
  if (options.engine != "inmemory" && options.engine != "rocksdb") {
    error = "--engine must be inmemory or rocksdb";
    return false;
  }
  if (!sitos::IsValidPrefix(options.prefix)) {
    error = "--prefix is invalid";
    return false;
  }
  if (options.engine == "rocksdb" && options.rocksdb_path.empty()) {
    error = "--rocksdb-path is required for rocksdb";
    return false;
  }
  if (options.engine == "inmemory" && options.rocksdb_path_seen) {
    error = "--rocksdb-path requires --engine rocksdb";
    return false;
  }
#ifndef SITOS_WITH_ROCKSDB
  if (options.engine == "rocksdb") {
    error = "rocksdb engine is unavailable in this build";
    return false;
  }
#endif
  return true;
}

bool ReadConfig(const std::string& path, std::string& content) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !content.empty() && !input.bad();
}

const auto ReportFailure = [](std::string_view operation, const auto& result) {
  std::string line = std::string(operation) +
                     " failed (status=" + std::to_string(static_cast<int>(result.StatusCode()));
  const std::string_view message = result.Message();
  if (!message.empty()) line += ", " + std::string(message);
  if (!result.IsOk()) {
    const auto& cause = result.Error();
    if (cause) {
      line += ", cause=";
      line += cause.category().name();
      line += ':';
      line += std::to_string(cause.value());
    }
  }
  line += ')';
  std::cerr << line << '\n';
};

using TransportResult = sitos::Result<std::unique_ptr<sitos::Transport>>;

TransportResult OpenConfiguredTransport(Options& options) {
  if (options.zenoh_config_file.has_value()) {
    std::string config;
    if (!ReadConfig(*options.zenoh_config_file, config)) {
      return TransportResult::Err(sitos::Status::InvalidArgument,
                                  "unable to read Zenoh configuration file");
    }
    // The borrowed view and its owner end in this scope immediately after the
    // synchronous factory call. Sitos never retains caller-owned JSON5 bytes.
    return sitos::OpenZenohTransport(std::string_view(config));
  }
  if (options.zenoh_config.has_value()) {
    std::string config = std::move(*options.zenoh_config);
    options.zenoh_config.reset();
    if (config.empty()) {
      return TransportResult::Err(sitos::Status::InvalidArgument,
                                  "Zenoh configuration must not be empty");
    }
    return sitos::OpenZenohTransport(std::string_view(config));
  }
  return sitos::OpenZenohTransport();
}

#ifdef _WIN32
HANDLE g_stop_event = nullptr;
BOOL WINAPI ConsoleHandler(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  return g_stop_event != nullptr && SetEvent(g_stop_event) != FALSE;
}
#else
volatile sig_atomic_t g_signal_pipe = -1;
void SignalHandler(int) {
  const int saved_errno = errno;
  const int descriptor = g_signal_pipe;
  if (descriptor >= 0) {
    const char notification = 's';
    const ssize_t write_result = ::write(descriptor, &notification, 1);
    (void)write_result;
  }
  errno = saved_errno;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    PrintHelp();
    return 0;
  }

  Options options;
  std::string parse_error;
  if (!ParseOptions(argc, argv, options, parse_error)) {
    std::cerr << "invalid arguments: " << parse_error << '\n';
    return 2;
  }

  auto opened_transport = OpenConfiguredTransport(options);
  if (!opened_transport.IsOk()) {
    ReportFailure("OpenZenohTransport", opened_transport);
    // A valid complete config can fail here when its selected listen port loses
    // a bind race. The standard-library driver retries this code only before readiness.
    return 3;
  }
  std::shared_ptr<sitos::Transport> transport(std::move(opened_transport).Value());

  std::shared_ptr<sitos::StorageEngine> engine;
#ifdef SITOS_WITH_ROCKSDB
  if (options.engine == "rocksdb") {
    auto opened_engine = sitos::RocksDBEngine::Open(options.rocksdb_path);
    if (!opened_engine.IsOk()) {
      ReportFailure("RocksDBEngine::Open", opened_engine);
      transport.reset();
      return 1;
    }
    engine = std::shared_ptr<sitos::StorageEngine>(std::move(opened_engine).Value());
  } else
#endif
  {
    engine = std::make_shared<sitos::InMemoryEngine>();
  }

  int exit_code = 0;
  {
    // StorageNode borrows transport. This scope guarantees destruction order:
    // node, then the caller's engine, then the shared Transport below.
    sitos::StorageNode node(*transport);
#ifdef _WIN32
    bool handler_registered = false;
    bool event_created = false;
    bool handler_ever_registered = false;
    bool handler_cleanup_failed = false;
    if (!SetConsoleCtrlHandler(nullptr, FALSE)) {
      std::cerr << "unable to re-enable Ctrl-C\n";
      exit_code = 1;
    } else {
      g_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
      event_created = g_stop_event != nullptr;
      if (!event_created || !SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        std::cerr << "unable to install console handler\n";
        if (event_created) {
          if (SetConsoleCtrlHandler(ConsoleHandler, FALSE)) {
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
            event_created = false;
          } else {
            handler_cleanup_failed = true;
          }
        }
        exit_code = 1;
      } else {
        handler_registered = true;
        handler_ever_registered = true;
      }
    }
#else
    int signal_pipe[2] = {-1, -1};
    struct sigaction previous_action {};
    bool handler_installed = false;
    if (pipe(signal_pipe) != 0) {
      std::cerr << "unable to create signal pipe\n";
      exit_code = 1;
    } else {
      const int flags = fcntl(signal_pipe[1], F_GETFL, 0);
      if (flags < 0 || fcntl(signal_pipe[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        std::cerr << "unable to configure signal pipe\n";
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        signal_pipe[0] = signal_pipe[1] = -1;
        exit_code = 1;
      } else {
        g_signal_pipe = signal_pipe[1];
        struct sigaction action {};
        action.sa_handler = SignalHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (sigaction(SIGINT, &action, &previous_action) != 0) {
          std::cerr << "unable to install SIGINT handler\n";
          g_signal_pipe = -1;
          close(signal_pipe[0]);
          close(signal_pipe[1]);
          signal_pipe[0] = signal_pipe[1] = -1;
          exit_code = 1;
        } else {
          handler_installed = true;
        }
      }
    }
#endif

    if (exit_code == 0) {
      auto started = node.Start(engine, {.prefix = options.prefix, .log_sink = nullptr});
      if (!started.IsOk()) {
        ReportFailure("StorageNode::Start", started);
        exit_code = 1;
      } else {
        std::cout << "SITOBOLON_READY " << options.prefix << std::endl;
#ifdef _WIN32
        const DWORD wait_result = WaitForSingleObject(g_stop_event, INFINITE);
        if (wait_result != WAIT_OBJECT_0) exit_code = 1;
#else
        char notification = 0;
        ssize_t read_result;
        do {
          read_result = read(signal_pipe[0], &notification, 1);
        } while (read_result < 0 && errno == EINTR);
        if (read_result != 1) exit_code = 1;
#endif
      }
    }

    // All node lifecycle work stays on the main path, outside the handlers.
    node.Stop();
#ifdef _WIN32
    if (handler_registered) {
      if (!SetConsoleCtrlHandler(ConsoleHandler, FALSE)) {
        std::cerr << "unable to remove console handler\n";
        exit_code = 1;
      } else {
        handler_registered = false;
      }
    }
    if (event_created && !handler_ever_registered && !handler_cleanup_failed) {
      CloseHandle(g_stop_event);
      g_stop_event = nullptr;
      event_created = false;
    } else if (handler_registered || handler_cleanup_failed) {
      // Keep the event reachable by the still-registered handler until process exit.
      exit_code = 1;
    }
    // Once registration succeeded, never close or reset g_stop_event. Even
    // after successful removal, an in-flight callback may still reference it;
    // process teardown owns that final handle close.
#else
    if (handler_installed) {
      if (sigaction(SIGINT, &previous_action, nullptr) != 0) {
        std::cerr << "unable to restore SIGINT handler\n";
        exit_code = 1;
      } else {
        g_signal_pipe = -1;
        handler_installed = false;
      }
    }
    // Do not close this pipe while a handler could still reference it. The
    // process exit closes it after restoration; this also covers restore failure.
#endif
  }

  // The node is gone before either owner below is reset. This is required for
  // RocksDB path release and for the StorageNode's borrowed Transport contract.
  engine.reset();
  transport.reset();
  return exit_code;
}
