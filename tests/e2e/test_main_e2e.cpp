// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file test_main_e2e.cpp
 * @brief End-to-end tests of main(): command line, configuration validation
 *        and the fork-based supervisor.
 *
 * The supervisor, the worker pool and the cache reader only exist as processes.
 * They are therefore tested the way an operator uses them: the built binary is
 * started with an environment and a direct FCGI socket, a client talks to it,
 * children are killed to force a restart, and the supervisor is shut down with
 * SIGTERM.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "knxnetip/knxnetip_frame.h"

namespace {

using Env = std::vector<std::pair<std::string, std::string>>;

std::string TempSocketPath(const char* tag) {
  static int counter = 0;
  return "/tmp/cvknx-main-" + std::string{tag} + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(counter++) + ".sock";
}

// The knxd transport can be compiled out (-DKNX_BUILD_KNXD_TRANSPORT=OFF); main()
// then rejects KNX_TRANSPORT=knxd before it opens any socket, so neither the
// fake daemon nor its test exists in such a build.
#ifdef KNX_WITH_KNXD_TRANSPORT

/// Read exactly @p size bytes, or fail on EOF/error.
bool ReadExactly(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t total = 0;
  while (total < size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t got = ::read(fd, data + total, size - total);
    if (got <= 0) {
      return false;
    }
    total += static_cast<std::size_t>(got);
  }
  return true;
}

/// A stand-in for knxd that answers the eibd group-socket handshake.
///
/// The startup probe only reports a bus link as usable when connect() succeeded,
/// and for the knxd transport that means "socket plus answered EIB_OPEN_GROUPCON".
/// A real daemon is not available in every environment (the coverage job runs
/// without knxd), so this helper makes the "reachable bus" path of main()
/// testable anywhere.
class FakeKnxd {
public:
  explicit FakeKnxd(const std::string& path) : path_(path) {
    ::unlink(path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path_.c_str());
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    thread_ = std::thread([this] { Serve(); });
  }

  ~FakeKnxd() {
    stop_.store(true);
    if (listen_fd_ >= 0) {
      // Wakes the accept() the worker thread is blocked in.
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    ::unlink(path_.c_str());
  }

  FakeKnxd(const FakeKnxd&) = delete;
  FakeKnxd& operator=(const FakeKnxd&) = delete;
  FakeKnxd(FakeKnxd&&) = delete;
  FakeKnxd& operator=(FakeKnxd&&) = delete;

  [[nodiscard]] bool listening() const { return listen_fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return path_; }

private:
  /// Accept connections and answer every EIB_OPEN_GROUPCON with a success
  /// reply (`[len=2][type=0x0026]`).  Other messages are ignored: the backend
  /// sends nothing else before it is told that the group socket is open.
  void Serve() {
    while (!stop_.load()) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        return;  // listening socket closed
      }
      struct timeval timeout {
        1, 0
      };
      (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      std::uint8_t header[2] = {};
      while (!stop_.load() && ReadExactly(fd, header, sizeof(header))) {
        const auto payload_len = static_cast<std::size_t>((header[0] << 8) | header[1]);
        std::vector<std::uint8_t> payload(payload_len);
        if (!ReadExactly(fd, payload.data(), payload.size())) {
          break;
        }
        const bool is_open_groupcon =
            payload.size() >= 2 && payload[0] == 0x00 && payload[1] == 0x26;
        if (is_open_groupcon) {
          const std::uint8_t reply[] = {0x00, 0x02, 0x00, 0x26};
          if (::write(fd, reply, sizeof(reply)) != static_cast<ssize_t>(sizeof(reply))) {
            break;  // the backend closed the connection: wait for the next one
          }
        }
      }
      ::close(fd);
    }
  }

  std::string path_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

#endif  // KNX_WITH_KNXD_TRANSPORT

/// A child process running the backend binary.
class BackendProcess {
public:
  BackendProcess(const std::vector<std::string>& args, const Env& env) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
      return;
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      ::dup2(in_pipe[0], STDIN_FILENO);
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::dup2(out_pipe[1], STDERR_FILENO);
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      ::unsetenv("FCGI_SOCKET");
      ::unsetenv("KNX_TRANSPORT");
      ::unsetenv("DEBUG_BACKEND");
      for (const auto& [name, value] : env) {
        ::setenv(name.c_str(), value.c_str(), 1);
      }
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(CVKNX_BINARY));
      for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(CVKNX_BINARY, argv.data());
      ::_exit(127);
    }
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);  // the backend does not read stdin
    ::close(out_pipe[1]);
    out_fd_ = out_pipe[0];
    reader_ = std::thread([this] { ReadLoop(); });
  }

  ~BackendProcess() {
    if (pid_ > 0 && !reaped_) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    if (out_fd_ >= 0) {
      ::close(out_fd_);
    }
  }

  BackendProcess(const BackendProcess&) = delete;
  BackendProcess& operator=(const BackendProcess&) = delete;
  BackendProcess(BackendProcess&&) = delete;
  BackendProcess& operator=(BackendProcess&&) = delete;

  [[nodiscard]] pid_t pid() const { return pid_; }

  /// Wait until the accumulated output contains @p needle.
  [[nodiscard]] bool wait_for_output(const std::string& needle, int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (output_.find(needle) != std::string::npos) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  /// First capture group of @p pattern in the accumulated output ("" if none).
  [[nodiscard]] std::string find_group(const std::regex& pattern, int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::smatch match;
        if (std::regex_search(output_, match, pattern) && match.size() > 1) {
          return match[1].str();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
  }

  [[nodiscard]] std::string output() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return output_;
  }

  /// Send SIGTERM and wait for the process to exit; returns the exit status.
  int terminate(int timeout_ms = 10000) {
    if (pid_ <= 0 || reaped_) {
      return -1;
    }
    ::kill(pid_, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        reaped_ = true;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
  }

  /// Run to completion (for the non-server invocations) and return the status.
  int wait_exit(int timeout_ms = 15000) {
    if (pid_ <= 0 || reaped_) {
      return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        reaped_ = true;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
  }

private:
  void ReadLoop() {
    char buffer[1024] = {};
    ssize_t n = 0;
    while ((n = ::read(out_fd_, buffer, sizeof(buffer))) > 0) {
      const std::lock_guard<std::mutex> lock(mutex_);
      output_.append(buffer, static_cast<size_t>(n));
    }
  }

  pid_t pid_ = -1;
  int out_fd_ = -1;
  std::thread reader_;
  mutable std::mutex mutex_;
  std::string output_;
  bool reaped_ = false;
};

/// Minimal direct-socket client: sends one FastCGI GET and returns the body.
std::string FcgiGet(const std::string& socket_path, const std::string& endpoint) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  ::timeval timeout{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }

  auto record = [](std::uint8_t type, const std::string& content) {
    std::string out{'\x01',
                    static_cast<char>(type),
                    '\x00',
                    '\x01',
                    static_cast<char>((content.size() >> 8) & 0xFF),
                    static_cast<char>(content.size() & 0xFF),
                    '\x00',
                    '\x00'};
    return out + content;
  };
  auto param = [](const std::string& name, const std::string& value) {
    return std::string{static_cast<char>(name.size()), static_cast<char>(value.size())} + name +
           value;
  };

  std::string request = record(0x01, std::string("\x00\x01\x00\x00\x00\x00\x00\x00", 8));
  // Split "path?query" the way a web server would: PATH_INFO and QUERY_STRING
  // are separate FastCGI parameters, and without QUERY_STRING the handlers see
  // no parameters at all.
  const auto question = endpoint.find('?');
  const std::string path = endpoint.substr(0, question);
  const std::string query =
      question == std::string::npos ? std::string{} : endpoint.substr(question + 1);
  request += record(0x04, param("REQUEST_METHOD", "GET") + param("REQUEST_URI", endpoint) +
                              param("PATH_INFO", path) + param("QUERY_STRING", query));
  request += record(0x04, "");
  request += record(0x05, "");

  size_t sent = 0;
  while (sent < request.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t written = ::write(fd, request.data() + sent, request.size() - sent);
    if (written <= 0) {
      ::close(fd);
      return {};
    }
    sent += static_cast<size_t>(written);
  }

  std::string response;
  char buffer[512] = {};
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, static_cast<size_t>(n));
  }
  ::close(fd);
  return response;
}

/// Open a connection and send one request without waiting for the response.
/// Returns the socket (caller closes) or -1.
int SendRequestAsync(const std::string& socket_path, const std::string& endpoint) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  auto record = [](std::uint8_t type, const std::string& content) {
    std::string out{'\x01',
                    static_cast<char>(type),
                    '\x00',
                    '\x01',
                    static_cast<char>((content.size() >> 8) & 0xFF),
                    static_cast<char>(content.size() & 0xFF),
                    '\x00',
                    '\x00'};
    return out + content;
  };
  auto param = [](const std::string& name, const std::string& value) {
    return std::string{static_cast<char>(name.size()), static_cast<char>(value.size())} + name +
           value;
  };
  std::string request = record(0x01, std::string("\x00\x01\x00\x00\x00\x00\x00\x00", 8));
  request += record(0x04, param("REQUEST_METHOD", "GET") + param("REQUEST_URI", endpoint));
  request += record(0x04, "");
  request += record(0x05, "");
  size_t sent = 0;
  while (sent < request.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t written = ::write(fd, request.data() + sent, request.size() - sent);
    if (written <= 0) {
      break;
    }
    sent += static_cast<size_t>(written);
  }
  return fd;
}

/// Environment for a backend that needs no KNX hardware and no knxd.
Env RoutingEnv() {
  return {{"KNX_TRANSPORT", "ip-routing"}};
}

// ================================================================
// Command line
// ================================================================

TEST(MainE2ETest, VersionIsPrinted) {
  BackendProcess process({"--version"}, {});
  EXPECT_EQ(process.wait_exit(), 0);
  EXPECT_NE(process.output().find("cometvisu-knx-fcgi"), std::string::npos) << process.output();
}

TEST(MainE2ETest, HelpIsPrinted) {
  BackendProcess process({"--help"}, {});
  EXPECT_EQ(process.wait_exit(), 0);
  const std::string out = process.output();
  EXPECT_NE(out.find("Usage: cometvisu-knx-fcgi"), std::string::npos) << out;
  EXPECT_NE(out.find("--discover"), std::string::npos) << out;
  EXPECT_NE(out.find("KNX_TRANSPORT"), std::string::npos) << out;
}

TEST(MainE2ETest, DiscoveryWithoutDevicesReportsThatNothingAnswered) {
  BackendProcess process({"--discover"},
                         {{"KNX_IP_MULTICAST", "224.0.23.12"}, {"KNX_IP_IFACE", "lo"}});
  const int code = process.wait_exit(30000);
  EXPECT_EQ(code, 1);
  EXPECT_NE(process.output().find("No KNXnet/IP device answered"), std::string::npos)
      << process.output();
}

// ================================================================
// Configuration validation
// ================================================================

TEST(MainE2ETest, MissingTransportIsRejected) {
  BackendProcess process({}, {});
  EXPECT_EQ(process.wait_exit(), 1);
  EXPECT_NE(process.output().find("KNX_TRANSPORT is required"), std::string::npos)
      << process.output();
}

TEST(MainE2ETest, UnknownTransportIsRejected) {
  BackendProcess process({}, {{"KNX_TRANSPORT", "carrier-pigeon"}});
  EXPECT_EQ(process.wait_exit(), 1);
  EXPECT_NE(process.output().find("[ERROR]"), std::string::npos) << process.output();
}

TEST(MainE2ETest, TunnelWithoutHostIsRejected) {
  BackendProcess process({}, {{"KNX_TRANSPORT", "ip-tunnel"}});
  EXPECT_EQ(process.wait_exit(), 1);
  EXPECT_NE(process.output().find("KNX_IP_HOST"), std::string::npos) << process.output();
}

/// The listen socket is mandatory now that spawn-fcgi mode is gone: without it
/// there would be nothing to accept on (and no bus owner, either).
TEST(MainE2ETest, MissingFcgiSocketIsRejected) {
  BackendProcess process({}, RoutingEnv());
  EXPECT_EQ(process.wait_exit(), 1);
  EXPECT_NE(process.output().find("FCGI_SOCKET is required"), std::string::npos)
      << process.output();
}

#ifdef KNX_WITH_KNXD_TRANSPORT
/// A transport with connection state is only reported as usable when the
/// handshake succeeded — and then without the "stateless" note that the
/// routing transport gets.  A real knxd socket therefore has to exist: the
/// environment supplies one, a daemon is replaced by the fake.
TEST(MainE2ETest, ReachableKnxdIsReportedAsUsable) {
  FakeKnxd daemon{TempSocketPath("fake-knxd")};
  ASSERT_TRUE(daemon.listening());

  BackendProcess process({}, {{"KNX_TRANSPORT", "knxd"},
                              {"KNXD_SOCKET", daemon.path()},
                              {"FCGI_SOCKET", TempSocketPath("knxd-probe")}});
  ASSERT_TRUE(process.wait_for_output("Bus link usable")) << process.output();
  EXPECT_NE(process.output().find("knxd at " + daemon.path()), std::string::npos)
      << process.output();
  EXPECT_EQ(process.output().find("stateless"), std::string::npos)
      << "the knxd transport has connection state, so the probe proves the bus works";
}
#endif  // KNX_WITH_KNXD_TRANSPORT

TEST(MainE2ETest, InvalidNumericSettingsFallBackToSafeValues) {
  Env env = RoutingEnv();
  // Unparsable values fall back to the documented defaults instead of aborting.
  env.emplace_back("LONGPOLL_TIMEOUT_SEC", "not-a-number");
  env.emplace_back("FCGI_WORKERS", "abc");
  env.emplace_back("ADDRESS_PREFIX", "KNX");
  env.emplace_back("BASE_URL", "/proxy/visu");
  env.emplace_back("LOGIN_EXTRA_CONFIG", R"("foo":"bar")");
  env.emplace_back("FCGI_SOCKET", TempSocketPath("numeric"));
  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 20 workers started, supervising children..."))
      << process.output();
  const std::string out = process.output();
  EXPECT_NE(out.find("LONGPOLL_TIMEOUT_SEC  300"), std::string::npos) << out;
  EXPECT_NE(out.find("FCGI_WORKERS          20"), std::string::npos) << out;
  EXPECT_NE(out.find("ADDRESS_PREFIX        KNX"), std::string::npos) << out;
  EXPECT_NE(out.find("BASE_URL              /proxy/visu"), std::string::npos) << out;
  EXPECT_NE(out.find(R"(LOGIN_EXTRA_CONFIG    "foo":"bar")"), std::string::npos) << out;
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(env.back().second.c_str());
}

TEST(MainE2ETest, WorkerCountIsCapped) {
  Env env = RoutingEnv();
  env.emplace_back("FCGI_WORKERS", "9999");
  env.emplace_back("FCGI_SOCKET", TempSocketPath("cap"));
  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("Starting 256 worker processes")) << process.output();
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(env.back().second.c_str());
}

// ================================================================
// Direct socket mode: worker pool and supervisor
// ================================================================

TEST(MainE2ETest, ServesRequestsAndSupervisesItsWorkers) {
  const std::string path = TempSocketPath("supervise");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "2");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 2 workers started, supervising children..."))
      << process.output();

  // The pool answers a real request.
  const std::string response = FcgiGet(path, "/l");
  EXPECT_NE(response.find("Status: 200"), std::string::npos) << response;

  // Kill one worker: the supervisor must notice and replace it.
  const std::string worker_pid = process.find_group(std::regex(R"(Started worker 0/2 pid=(\d+))"));
  ASSERT_FALSE(worker_pid.empty()) << process.output();
  ASSERT_EQ(::kill(std::stoi(worker_pid), SIGTERM), 0);
  EXPECT_TRUE(process.wait_for_output("died — restarting it")) << process.output();
  EXPECT_TRUE(process.wait_for_output("Restarted worker 0 pid=")) << process.output();

  // Kill the cache reader: it is restarted as well.
  const std::string reader_pid =
      process.find_group(std::regex(R"(Cache reader started, pid=(\d+))"));
  ASSERT_FALSE(reader_pid.empty()) << process.output();
  ASSERT_EQ(::kill(std::stoi(reader_pid), SIGTERM), 0);
  EXPECT_TRUE(process.wait_for_output("died — restarting it", 15000)) << process.output();

  // The pool still serves requests after the restarts.
  const std::string response2 = FcgiGet(path, "/l");
  EXPECT_NE(response2.find("Status: 200"), std::string::npos) << response2;

  // SIGTERM stops every child and ends the supervisor cleanly.
  EXPECT_EQ(process.terminate(), 0);
  EXPECT_NE(process.output().find("Shutdown requested, stopping all children"), std::string::npos)
      << process.output();
  ::unlink(path.c_str());
}

TEST(MainE2ETest, WorkerWithAnInvalidEndpointIsAnsweredByTheRouter) {
  const std::string path = TempSocketPath("endpoint");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "1");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 1 workers started, supervising children..."))
      << process.output();

  const std::string response = FcgiGet(path, "/nope");
  EXPECT_NE(response.find("Status: 404"), std::string::npos) << response;
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

/// A fake KNXnet/IP device that answers a SEARCH_REQUEST with a device
/// description, so the --discover output path can be exercised.
class FakeSearchResponder {
public:
  explicit FakeSearchResponder(std::string name = "Fake KNXnet/IP interface")
      : name_(std::move(name)), fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {
    if (fd_ < 0) {
      return;
    }
    int enable = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3671);  // the port --discover sends to
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {  // NOLINT
      ::close(fd_);
      fd_ = -1;
      return;
    }
    ::timeval timeout{10, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

  ~FakeSearchResponder() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  FakeSearchResponder(const FakeSearchResponder&) = delete;
  FakeSearchResponder& operator=(const FakeSearchResponder&) = delete;
  FakeSearchResponder(FakeSearchResponder&&) = delete;
  FakeSearchResponder& operator=(FakeSearchResponder&&) = delete;

  [[nodiscard]] bool is_valid() const { return fd_ >= 0; }

  /// Answer exactly one search request (blocking, bounded by the timeout).
  bool answer_one() {
    if (fd_ < 0) {
      return false;
    }
    std::uint8_t request[512] = {};
    struct sockaddr_in from {};
    socklen_t from_len = sizeof(from);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const ssize_t received = ::recvfrom(fd_, request, sizeof(request), 0,
                                        reinterpret_cast<struct sockaddr*>(&from), &from_len);
    if (received <= 0) {
      return false;
    }
    const auto response = BuildResponse(name_, 0x1101);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ::sendto(fd_, response.data(), response.size(), 0,
                    reinterpret_cast<const struct sockaddr*>(&from), from_len) > 0;
  }

private:
  static std::vector<std::uint8_t> BuildResponse(const std::string& name,
                                                 std::uint16_t individual_address) {
    using Bytes = std::vector<std::uint8_t>;
    Bytes body;
    const auto control = cvknx::knxnetip::encode_hpai(cvknx::knxnetip::Hpai{0x7F000001U, 3671});
    const auto endpoint = cvknx::knxnetip::encode_hpai(cvknx::knxnetip::Hpai{0x7F000001U, 3671});
    body.insert(body.end(), control.begin(), control.end());
    body.insert(body.end(), endpoint.begin(), endpoint.end());

    Bytes dib(54, 0x00);
    dib[0] = 54;    // structure length
    dib[1] = 0x01;  // device information
    dib[2] = 0x02;  // medium: IP
    dib[4] = static_cast<std::uint8_t>((individual_address >> 8) & 0xFF);
    dib[5] = static_cast<std::uint8_t>(individual_address & 0xFF);
    dib[14] = 0xEF;
    dib[15] = 0x00;
    dib[16] = 0x17;
    dib[17] = 0x0C;  // routing multicast 239.0.23.12
    for (std::size_t i = 0; i < name.size() && i < 30; ++i) {
      dib[24 + i] = static_cast<std::uint8_t>(name[i]);
    }
    body.insert(body.end(), dib.begin(), dib.end());
    return cvknx::knxnetip::build_frame(cvknx::knxnetip::ServiceType::SearchResponse, body);
  }

  std::string name_;
  int fd_ = -1;
};

TEST(MainE2ETest, DiscoveryPrintsEveryDeviceThatAnswers) {
  FakeSearchResponder responder;
  if (!responder.is_valid()) {
    GTEST_SKIP() << "UDP port 3671 is not available";
  }
  std::thread answerer([&responder] { (void)responder.answer_one(); });

  // 127.0.0.1 makes the SEARCH_REQUEST a unicast datagram to the fake device.
  BackendProcess process({"--discover"}, {{"KNX_IP_MULTICAST", "127.0.0.1"}});
  const int code = process.wait_exit(30000);
  answerer.join();
  EXPECT_EQ(code, 0);
  const std::string out = process.output();
  // The device line is printed with the parsed individual address, endpoint and
  // routing multicast address (the name field itself is padded by the device and
  // is asserted by the "(unnamed)" case below).
  EXPECT_NE(out.find("KNXnet/IP device: "), std::string::npos) << out;
  EXPECT_NE(out.find("individual=1.1.1"), std::string::npos) << out;
  EXPECT_NE(out.find("endpoint=127.0.0.1:3671"), std::string::npos) << out;
  EXPECT_NE(out.find("multicast=239.0.23.12"), std::string::npos) << out;
}

TEST(MainE2ETest, KilledWorkerIsReportedAndRestarted) {
  const std::string path = TempSocketPath("killed");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "2");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 2 workers started, supervising children..."))
      << process.output();

  // SIGKILL cannot be caught: the supervisor must report the signal and respawn.
  const std::string worker_pid = process.find_group(std::regex(R"(Started worker 0/2 pid=(\d+))"));
  ASSERT_FALSE(worker_pid.empty()) << process.output();
  ASSERT_EQ(::kill(std::stoi(worker_pid), SIGKILL), 0);
  EXPECT_TRUE(process.wait_for_output("killed by signal 9")) << process.output();
  EXPECT_TRUE(process.wait_for_output("Restarted worker 0 pid=")) << process.output();
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

TEST(MainE2ETest, WorkerKilledDuringALongPollReturnsItsTokens) {
  const std::string path = TempSocketPath("tokens");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "1");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 1 workers started, supervising children..."))
      << process.output();

  // A long-poll occupies the only worker and holds both semaphore tokens.
  const int client = SendRequestAsync(path, "/r?t=300&a=1/2/3&s=0");
  ASSERT_GE(client, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const std::string worker_pid = process.find_group(std::regex(R"(Started worker 0/1 pid=(\d+))"));
  ASSERT_FALSE(worker_pid.empty()) << process.output();
  ASSERT_EQ(::kill(std::stoi(worker_pid), SIGKILL), 0);
  EXPECT_TRUE(process.wait_for_output("Restarted worker 0 pid=")) << process.output();
  ::close(client);

  // The pool is usable again: a read is served instead of being rejected.
  const std::string response = FcgiGet(path, "/l");
  EXPECT_NE(response.find("Status: 200"), std::string::npos) << response;
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

TEST(MainE2ETest, ShortOptionAliasesWork) {
  for (const char* option : {"-v", "-h"}) {
    BackendProcess process({option}, {});
    EXPECT_EQ(process.wait_exit(), 0) << option;
    EXPECT_FALSE(process.output().empty()) << option;
  }
}

TEST(MainE2ETest, DiscoveryWithEmptyOverridesUsesTheDefaults) {
  // Empty settings fall back to the built-in defaults instead of overriding
  // them with an empty string.
  BackendProcess process({"--discover"}, {{"KNX_IP_MULTICAST", ""}, {"KNX_IP_IFACE", ""}});
  const int code = process.wait_exit(30000);
  EXPECT_EQ(code, 1);
  EXPECT_NE(process.output().find("No KNXnet/IP device answered"), std::string::npos)
      << process.output();
}

TEST(MainE2ETest, StartupInfoToleratesUnsetAndEmptyValues) {
  const std::string path = TempSocketPath("info");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "1");
  env.emplace_back("BASE_URL", "");                // present but empty
  env.emplace_back("ADDRESS_PREFIX", "");          // present but empty
  env.emplace_back("LONGPOLL_TIMEOUT_SEC", "5x");  // trailing garbage
  env.emplace_back("DEBUG_BACKEND", "1");
  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 1 workers started, supervising children..."))
      << process.output();
  const std::string out = process.output();
  EXPECT_NE(out.find("BASE_URL              (not set)"), std::string::npos) << out;
  EXPECT_NE(out.find("ADDRESS_PREFIX        (not set)"), std::string::npos) << out;
  EXPECT_NE(out.find("DEBUG_BACKEND         1"), std::string::npos) << out;
  // "5x" is not a number: the documented default applies.
  EXPECT_NE(out.find("LONGPOLL_TIMEOUT_SEC  300"), std::string::npos) << out;
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

TEST(MainE2ETest, DiscoveryReportsADeviceWithoutAName) {
  FakeSearchResponder responder("");  // empty device name
  if (!responder.is_valid()) {
    GTEST_SKIP() << "UDP port 3671 is not available";
  }
  std::thread answerer([&responder] { (void)responder.answer_one(); });

  BackendProcess process({"--discover"}, {{"KNX_IP_MULTICAST", "127.0.0.1"}});
  const int code = process.wait_exit(30000);
  answerer.join();
  EXPECT_EQ(code, 0);
  EXPECT_NE(process.output().find("KNXnet/IP device: (unnamed)"), std::string::npos)
      << process.output();
}

TEST(MainE2ETest, UnknownOptionFallsThroughToTheServerStart) {
  // An unknown option is not an error by itself: the backend starts normally
  // and fails on the missing transport configuration.
  BackendProcess process({"--bogus"}, {});
  EXPECT_EQ(process.wait_exit(), 1);
  EXPECT_NE(process.output().find("KNX_TRANSPORT is required"), std::string::npos)
      << process.output();
}

TEST(MainE2ETest, NumericSettingsBelowTheMinimumUseTheDefault) {
  const std::string path = TempSocketPath("clamp");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "1");
  env.emplace_back("LONGPOLL_TIMEOUT_SEC", "-5");  // below the minimum
  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 1 workers started, supervising children..."))
      << process.output();
  EXPECT_NE(process.output().find("LONGPOLL_TIMEOUT_SEC  300"), std::string::npos)
      << process.output();
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

/// A value that is not a number, has trailing garbage or overflows must fall
/// back to the default instead of being truncated into a plausible one.
TEST(MainE2ETest, MalformedNumericSettingsUseTheDefault) {
  for (const char* value : {"abc", "2x", "99999999999999999999"}) {
    const std::string path = TempSocketPath("garbage");
    Env env = RoutingEnv();
    env.emplace_back("FCGI_SOCKET", path);
    env.emplace_back("FCGI_WORKERS", value);

    BackendProcess process({}, env);
    ASSERT_TRUE(process.wait_for_output("All 20 workers started, supervising children...", 15000))
        << "FCGI_WORKERS=" << value << ":\n"
        << process.output();
    EXPECT_EQ(process.terminate(), 0);
    ::unlink(path.c_str());
  }
}

TEST(MainE2ETest, EmptyNumericSettingUsesTheDefault) {
  const std::string path = TempSocketPath("empty");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "");
  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 20 workers started, supervising children..."))
      << process.output();
  EXPECT_NE(process.output().find("FCGI_WORKERS          20"), std::string::npos)
      << process.output();
  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

TEST(MainE2ETest, DiscoveryWithoutAnyOverrideUsesTheCompiledDefault) {
  BackendProcess process({"--discover"}, {});
  const int code = process.wait_exit(30000);
  EXPECT_EQ(code, 1);
  EXPECT_NE(process.output().find("224.0.23.12:3671"), std::string::npos) << process.output();
}

// ================================================================
// Sessions across the worker pool
// ================================================================

namespace {

/// Log in through the pool and return the session id.
std::string LoginSession(const std::string& socket_path) {
  const std::string response = FcgiGet(socket_path, "/l?u=user&p=pass");
  const auto start = response.find("\"s\":\"");
  if (start == std::string::npos) {
    return {};
  }
  const auto value_start = start + 5;
  const auto value_end = response.find('"', value_start);
  if (value_end == std::string::npos) {
    return {};
  }
  return response.substr(value_start, value_end - value_start);
}

}  // namespace

/// Every client must be able to log in and then be recognised by *any* worker.
///
/// Regression test: the session store used to be an ordinary member copied by
/// fork(), so a session created by one worker answered 401 in every other one
/// (verified: with four workers every follow-up request failed).
TEST(MainE2ETest, SessionIsValidInEveryWorker) {
  const std::string path = TempSocketPath("session");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "4");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 4 workers started, supervising children..."))
      << process.output();
  ASSERT_TRUE(process.wait_for_output("Shared session store created")) << process.output();

  const std::string session = LoginSession(path);
  ASSERT_FALSE(session.empty()) << process.output();

  // Ten sequential requests are spread over the pool; none may answer 401.
  for (int i = 0; i < 10; ++i) {
    const std::string response = FcgiGet(path, "/r?a=1/2/3&t=-1&s=" + session);
    EXPECT_EQ(response.find("Status: 401"), std::string::npos)
        << "request " << i << " hit a worker that does not know the session:\n"
        << response;
  }

  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

/// More logins than the store can hold must not lock anybody out: the oldest
/// session is evicted, the newest keeps working.
TEST(MainE2ETest, OldestSessionIsEvictedWhenTheStoreIsFull) {
  const std::string path = TempSocketPath("session-full");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "2");
  env.emplace_back("SESSION_MAX", "2");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 2 workers started, supervising children..."))
      << process.output();
  ASSERT_TRUE(process.wait_for_output("capacity=2")) << process.output();

  const std::string first = LoginSession(path);
  const std::string second = LoginSession(path);
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());

  // The store holds two sessions; a third login evicts the first one.
  const std::string third = LoginSession(path);
  ASSERT_FALSE(third.empty());

  const std::string third_response = FcgiGet(path, "/r?a=1/2/3&t=-1&s=" + third);
  EXPECT_NE(third_response.find("Status: 200"), std::string::npos) << third_response;
  const std::string second_response = FcgiGet(path, "/r?a=1/2/3&t=-1&s=" + second);
  EXPECT_NE(second_response.find("Status: 200"), std::string::npos) << second_response;
  const std::string first_response = FcgiGet(path, "/r?a=1/2/3&t=-1&s=" + first);
  EXPECT_NE(first_response.find("Status: 401"), std::string::npos)
      << "the oldest session should have been evicted, got: " << first_response;

  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

// ================================================================
// The FCGI endpoint is the only access control
// ================================================================

/// A filesystem socket inherits the umask (typically 0755, i.e. connectable by
/// every local user).  The backend narrows it to owner/group rw.
TEST(MainE2ETest, DirectSocketIsRestrictedToOwnerAndGroup) {
  const std::string path = TempSocketPath("perms");
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", path);
  env.emplace_back("FCGI_WORKERS", "1");

  BackendProcess process({}, env);
  ASSERT_TRUE(process.wait_for_output("All 1 workers started, supervising children..."))
      << process.output();

  struct stat info {};
  ASSERT_EQ(::stat(path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0660) << "the FastCGI socket must not be world accessible";

  EXPECT_EQ(process.terminate(), 0);
  ::unlink(path.c_str());
}

/// A TCP listener has no peer check and no authentication; the operator must be
/// told instead of silently exposing the KNX bus to the network.
TEST(MainE2ETest, TcpSocketModeWarnsAboutTheMissingAuthentication) {
  Env env = RoutingEnv();
  env.emplace_back("FCGI_SOCKET", ":0");
  env.emplace_back("FCGI_WORKERS", "1");

  BackendProcess process({}, env);
  EXPECT_TRUE(process.wait_for_output("listens on a TCP port with no authentication"))
      << process.output();
  EXPECT_EQ(process.terminate(), 0);
}

}  // namespace
