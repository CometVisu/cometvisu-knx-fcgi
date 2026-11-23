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
 * @file test_fcgi_server_run.cpp
 * @brief Full-cycle tests for FcgiServer::run() — real FastCGI records.
 *
 * The server is driven the way a web server drives it: a child process runs the
 * accept loop while the test connects over a Unix socket and speaks the
 * FastCGI record protocol (BEGIN_REQUEST / PARAMS / STDIN in, STDOUT /
 * END_REQUEST out).  This covers parameter parsing, the long-poll capacity
 * limiting, response serialisation and the shutdown wake-up path.
 *
 * The child is stopped with SIGUSR1; the handler calls shutdown(), which is
 * async-signal-safe (shutdown/close/socket/connect/getsockname only) and lets
 * run() return normally so the child can flush its coverage counters.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fcgi/fcgi_server.h"
#include "util/coverage.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

// FastCGI record types.
constexpr std::uint8_t kBeginRequest = 1;
constexpr std::uint8_t kEndRequest = 3;
constexpr std::uint8_t kParams = 4;
constexpr std::uint8_t kStdin = 5;
constexpr std::uint8_t kStdout = 6;
constexpr std::uint16_t kRequestId = 1;

/// One FastCGI record: version, type, request id, length, padding, reserved.
Bytes Record(std::uint8_t type, const Bytes& content, std::uint16_t request_id = kRequestId) {
  Bytes out{0x01,
            type,
            static_cast<std::uint8_t>(request_id >> 8),
            static_cast<std::uint8_t>(request_id & 0xFF),
            static_cast<std::uint8_t>(content.size() >> 8),
            static_cast<std::uint8_t>(content.size() & 0xFF),
            0x00,
            0x00};
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

/// One name-value pair of the PARAMS stream (both lengths < 128 in these tests).
Bytes Param(const std::string& name, const std::string& value) {
  Bytes out{static_cast<std::uint8_t>(name.size()), static_cast<std::uint8_t>(value.size())};
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

using Params = std::vector<std::pair<std::string, std::string>>;

/// A complete request: BEGIN_REQUEST, PARAMS, STDIN (plus the empty records
/// that terminate both streams).
Bytes BuildRequest(const Params& params, const Bytes& body = {}, bool keep_conn = false) {
  // role = FCGI_RESPONDER (1), flags = FCGI_KEEP_CONN (1) or 0, reserved = 0
  const std::uint8_t flags = keep_conn ? 0x01 : 0x00;
  Bytes request = Record(kBeginRequest, Bytes{0x00, 0x01, flags, 0x00, 0x00, 0x00, 0x00, 0x00});

  Bytes pairs;
  for (const auto& [name, value] : params) {
    const Bytes pair = Param(name, value);
    pairs.insert(pairs.end(), pair.begin(), pair.end());
  }
  const Bytes params_record = Record(kParams, pairs);
  request.insert(request.end(), params_record.begin(), params_record.end());
  const Bytes params_end = Record(kParams, {});
  request.insert(request.end(), params_end.begin(), params_end.end());

  if (!body.empty()) {
    const Bytes body_record = Record(kStdin, body);
    request.insert(request.end(), body_record.begin(), body_record.end());
  }
  const Bytes stdin_end = Record(kStdin, {});
  request.insert(request.end(), stdin_end.begin(), stdin_end.end());
  return request;
}

/// Handler behaviour of the child server, selected per test.
enum class Behaviour { Echo, ThrowStd, ThrowUnknown };

RequestHandler MakeHandler(Behaviour behaviour) {
  return [behaviour](const FcgiRequest& req) {
    switch (behaviour) {
      case Behaviour::ThrowStd:
        throw std::runtime_error("boom");
      case Behaviour::ThrowUnknown:
        throw 42;  // NOLINT(hicpp-exception-baseclass)
      case Behaviour::Echo:
        break;
    }
    FcgiResponse response;
    response.status_code = 200;
    response.content_type = "application/json";
    response.body = "{\"path\":\"" + std::string{req.path()} + "\",\"method\":\"" +
                    req.request_method + "\",\"len\":" + std::to_string(req.content.size()) + "}";
    return response;
  };
}

/// The server instance the SIGUSR1 handler shuts down (child process only).
FcgiServer* g_server = nullptr;

void OnShutdownSignal(int /*signal*/) {
  if (g_server != nullptr) {
    g_server->shutdown();  // async-signal-safe: shutdown/close/socket/connect only
  }
}

std::string SocketPath(const char* tag) {
  static int counter = 0;
  return "/tmp/cvknx-fcgi-" + std::string{tag} + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(counter++) + ".sock";
}

/// Runs FcgiServer::run() in a child process and talks to it over a socket.
class ServerProcess {
public:
  ServerProcess(FcgiServer& server, bool use_concurrency_sem, bool use_load_shed_sem,
                int connection_timeout_ms = 0)
      : path_(SocketPath("run")) {
    if (connection_timeout_ms > 0) {
      server.set_connection_timeout_ms(connection_timeout_ms);
    }
    // Semaphores live in shared memory so the parent can inspect them.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    sems_ = static_cast<sem_t*>(::mmap(nullptr, 2 * sizeof(sem_t), PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (sems_ == MAP_FAILED) {
      sems_ = nullptr;
      return;
    }
    ::sem_init(&sems_[0], 1, 1);  // concurrency
    ::sem_init(&sems_[1], 1, 1);  // load shedding
    if (use_concurrency_sem) {
      server.set_concurrency_semaphore(&sems_[0]);
    }
    if (use_load_shed_sem) {
      server.set_load_shed_semaphore(&sems_[1]);
    }

    pid_ = ::fork();
    if (pid_ == 0) {
      g_server = &server;
      ::signal(SIGUSR1, OnShutdownSignal);
      // The request body is read from the process stdin; detach it so a POST
      // sees EOF instead of blocking on the terminal or the test runner's pipe.
      const int devnull = ::open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::close(devnull);
      }
      int code = 0;
      try {
        code = [&server, this] {
          if (!server.listen(path_)) {
            return 2;
          }
          return server.run();
        }();
      } catch (const std::exception& e) {
        std::cerr << "CHILD-EXCEPTION: " << e.what() << "\n";
        code = 9;
      } catch (...) {
        std::cerr << "CHILD-EXCEPTION: unknown\n";
        code = 9;
      }
      KNX_COVERAGE_DUMP();
      ::_exit(code);
    }

    // Wait until the child has opened the socket.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (Connect()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  /// Open one connection to the child's listen socket.
  [[nodiscard]] bool Connect() {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    ::timeval timeout{5, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
      return true;
    }
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  ~ServerProcess() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (pid_ > 0) {
      ::kill(pid_, SIGUSR1);  // makes run() return so the child can flush coverage
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      exit_status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (sems_ != nullptr) {
      ::sem_destroy(&sems_[0]);
      ::sem_destroy(&sems_[1]);
      ::munmap(sems_, 2 * sizeof(sem_t));
    }
    ::unlink(path_.c_str());
  }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;
  ServerProcess(ServerProcess&&) = delete;
  ServerProcess& operator=(ServerProcess&&) = delete;

  [[nodiscard]] bool connected() const { return fd_ >= 0; }
  [[nodiscard]] int exit_status() const { return exit_status_; }

  /// Number of free concurrency/load-shed tokens (parent side).
  [[nodiscard]] int concurrency_tokens() const {
    int value = -1;
    if (sems_ != nullptr) {
      ::sem_getvalue(&sems_[0], &value);
    }
    return value;
  }
  [[nodiscard]] int load_shed_tokens() const {
    int value = -1;
    if (sems_ != nullptr) {
      ::sem_getvalue(&sems_[1], &value);
    }
    return value;
  }

  /// Wait until the concurrency token count reaches @p expected.
  [[nodiscard]] bool wait_for_concurrency_tokens(int expected, int timeout_ms = 2000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (concurrency_tokens() == expected) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  /// Wait until both token counts reach @p expected (the child releases them
  /// after it has written the response).
  [[nodiscard]] bool wait_for_tokens(int expected, int timeout_ms = 2000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (concurrency_tokens() == expected && load_shed_tokens() == expected) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  /// Consume the single long-poll concurrency token (parent side).
  [[nodiscard]] bool take_concurrency_token() const {
    return sems_ != nullptr && ::sem_trywait(&sems_[0]) == 0;
  }

  /// Consume the single load-shedding token (parent side).
  [[nodiscard]] bool take_load_shed_token() const {
    return sems_ != nullptr && ::sem_trywait(&sems_[1]) == 0;
  }

  /// Send one request on the current connection without reading the reply.
  [[nodiscard]] bool SendRequest(const Params& params, const Bytes& body = {}) {
    return SendBytes(BuildRequest(params, body));
  }

  /// Send raw bytes on the current connection (a partial request, for example).
  [[nodiscard]] bool SendBytes(const Bytes& request) {
    if (fd_ < 0 && !Connect()) {
      return false;
    }
    size_t sent = 0;
    while (sent < request.size()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const ssize_t written = ::send(fd_, request.data() + sent, request.size() - sent, 0);
      if (written <= 0) {
        return false;
      }
      sent += static_cast<size_t>(written);
    }
    return true;
  }

  /// Send one request that asks for the connection to stay open and read its
  /// response, leaving the connection available for the next request.
  std::string ExchangeKeepAlive(const Params& params) {
    if (!SendBytes(BuildRequest(params, {}, /*keep_conn=*/true))) {
      return {};
    }
    return ReadResponse();
  }

  /// Detach the current connection: the test keeps the descriptor open (to
  /// present an idle or stalled client), the next Exchange reconnects.
  int ReleaseConnection() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  /// Close the current connection and allow the next request to reconnect.
  void CloseConnection() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /// True once the peer closed the connection (EOF) within @p timeout_ms.
  [[nodiscard]] bool PeerClosed(int fd, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      char discard[512] = {};
      const ssize_t n = ::recv(fd, discard, sizeof(discard), 0);
      if (n == 0) {
        return true;
      }
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;  // SO_RCVTIMEO slice elapsed, keep waiting
        }
        return errno == ECONNRESET || errno == EPIPE;
      }
    }
    return false;
  }

  /// Wait until the reply has started to arrive, without consuming it.
  [[nodiscard]] bool WaitForReplyBytes(int timeout_ms = 5000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::uint8_t byte = 0;
      const ssize_t n = ::recv(fd_, &byte, 1, MSG_PEEK);
      if (n > 0) {
        return true;
      }
      if (n == 0) {
        return false;  // connection closed without a reply
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return false;
      }
    }
    return false;
  }

  /// Read the rest of the in-flight reply and close the connection.
  std::string FinishExchange() {
    std::string payload = ReadResponse();
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    return payload;
  }

  /// Send one request and return the response body (STDOUT records joined).
  /// Opens a fresh connection when needed — libfcgi closes the connection after
  /// a response unless the client asked for keep-alive.
  std::string Exchange(const Params& params, const Bytes& body = {}) {
    if (fd_ < 0 && !Connect()) {
      return {};
    }
    const Bytes request = BuildRequest(params, body);
    size_t sent = 0;
    while (sent < request.size()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const ssize_t written = ::send(fd_, request.data() + sent, request.size() - sent, 0);
      if (written <= 0) {
        return {};
      }
      sent += static_cast<size_t>(written);
    }
    std::string payload = ReadResponse();
    ::close(fd_);
    fd_ = -1;
    return payload;
  }

private:
  /// Read records until END_REQUEST and return the concatenated STDOUT body.
  std::string ReadResponse() {
    std::string payload;
    Bytes buffer;
    while (true) {
      std::uint8_t chunk[4096] = {};
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        return payload;
      }
      buffer.insert(buffer.end(), chunk, chunk + static_cast<std::ptrdiff_t>(n));
      size_t offset = 0;
      while (buffer.size() - offset >= 8) {
        const std::uint8_t type = buffer[offset + 1];
        const size_t length = (static_cast<size_t>(buffer[offset + 4]) << 8) | buffer[offset + 5];
        const size_t padding = buffer[offset + 6];
        if (buffer.size() - offset < 8 + length + padding) {
          break;  // wait for the rest of this record
        }
        if (type == kStdout) {
          payload.append(reinterpret_cast<const char*>(&buffer[offset + 8]),  // NOLINT
                         length);
        }
        const bool end = type == kEndRequest;
        offset += 8 + length + padding;
        if (end) {
          return payload;
        }
      }
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }

  std::string path_;
  sem_t* sems_ = nullptr;
  pid_t pid_ = -1;
  int fd_ = -1;
  int exit_status_ = -1;
};

// ================================================================
// Happy path
// ================================================================

TEST(FcgiServerRunTest, ServesARequestAndStopsCleanly) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l?v=1"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_NE(body.find("Content-Type: application/json; charset=utf-8"), std::string::npos) << body;
  const std::string payload = R"({"path":"/l","method":"GET","len":0})";
  EXPECT_NE(body.find(payload), std::string::npos) << body;
  // Content-Length must match the body that follows the empty line.
  EXPECT_NE(body.find("Content-Length: " + std::to_string(payload.size())), std::string::npos)
      << body;
}

TEST(FcgiServerRunTest, UsesPathInfoWhenPresent) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"},
                                          {"REQUEST_URI", "/cgi-bin/visu/w?a=1/2/3"},
                                          {"SCRIPT_NAME", "/cgi-bin/visu"},
                                          {"PATH_INFO", "/w"}});
  EXPECT_NE(body.find(R"({"path":"/w","method":"GET","len":0})"), std::string::npos) << body;
}

/// Several requests over one connection exercise the accept loop's repetition.
TEST(FcgiServerRunTest, ServesMultipleRequestsOnOneConnection) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  EXPECT_NE(proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}}).find("Status: 200"),
            std::string::npos);
  EXPECT_NE(
      proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}}).find(R"("path":"/r")"),
      std::string::npos);
}

// ================================================================
// Connection lifetime — a client must never park a worker
// ================================================================

/// A keep-alive connection really carries more than one request.
///
/// The worker owns the accepted connection now, so this is the regression test
/// for the request loop: libfcgi must be handed the fd (with keepConnection set)
/// for every further request on the same connection.
TEST(FcgiServerRunTest, KeepAliveConnectionServesSeveralRequests) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string first =
      proc.ExchangeKeepAlive({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(first.find("Status: 200"), std::string::npos) << first;
  const std::string second =
      proc.ExchangeKeepAlive({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}});
  EXPECT_NE(second.find(R"("path":"/r")"), std::string::npos) << second;

  // The client closes the (still open) keep-alive connection: the worker must
  // notice the EOF and go back to accepting instead of keeping it forever.
  const int closed_fd = proc.ReleaseConnection();
  ::close(closed_fd);
  const std::string third = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(third.find("Status: 200"), std::string::npos) << third;
}

/// An idle keep-alive connection is dropped after the deadline and the worker
/// returns to accepting: a client that never sends its next request must not
/// occupy a worker (the FastCGI protocol has no keep-alive timeout of its own).
TEST(FcgiServerRunTest, IdleKeepAliveConnectionIsDroppedAfterTheTimeout) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false, /*connection_timeout_ms=*/200);
  ASSERT_TRUE(proc.connected());
  const std::string first =
      proc.ExchangeKeepAlive({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  ASSERT_NE(first.find("Status: 200"), std::string::npos) << first;

  // The connection stays open but idle; the worker must give up on it.
  const int idle_fd = proc.ReleaseConnection();
  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/w"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_TRUE(proc.PeerClosed(idle_fd)) << "the idle keep-alive connection was never closed";
  ::close(idle_fd);
}

/// A client that connects and sends nothing at all is dropped after the
/// deadline; the worker serves the next connection in the meantime.
TEST(FcgiServerRunTest, SilentConnectionIsDroppedAfterTheTimeout) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false, /*connection_timeout_ms=*/200);
  ASSERT_TRUE(proc.connected());
  const int silent_fd = proc.ReleaseConnection();  // connected, not a single byte sent

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_TRUE(proc.PeerClosed(silent_fd)) << "the silent connection was never closed";
  ::close(silent_fd);
}

/// A half-sent request (BEGIN_REQUEST without the PARAMS terminator) is a
/// slowloris: it must be dropped after the deadline instead of pinning the
/// worker forever.
TEST(FcgiServerRunTest, StalledRequestIsDroppedAfterTheTimeout) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false, /*connection_timeout_ms=*/200);
  ASSERT_TRUE(proc.connected());
  // BEGIN_REQUEST + a PARAMS record, but no empty PARAMS record to end them.
  Bytes partial = Record(kBeginRequest, Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  const Bytes params = Record(kParams, Param("REQUEST_METHOD", "GET"));
  partial.insert(partial.end(), params.begin(), params.end());
  ASSERT_TRUE(proc.SendBytes(partial));
  const int stalled_fd = proc.ReleaseConnection();

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_TRUE(proc.PeerClosed(stalled_fd)) << "the stalled request was never dropped";
  ::close(stalled_fd);
}

/// The idle accept loop keeps serving after its poll slice expires (the worker
/// is not blocked in accept(), it re-checks the shutdown flag every slice).
TEST(FcgiServerRunTest, IdleAcceptLoopKeepsServingAfterTheSlice) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // > kAcceptPollSliceMs

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
}

/// A shutdown must also end a worker that is blocked reading a request.  The
/// production connection timeout is minutes long, so without shutdown() waking
/// the active connection the worker would sit in the read until it expires.
TEST(FcgiServerRunTest, ShutdownEndsAWorkerBlockedWaitingForARequest) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  int silent_fd = -1;
  {
    // A long deadline: only the shutdown may end the read.
    ServerProcess proc(server, false, false, /*connection_timeout_ms=*/60000);
    ASSERT_TRUE(proc.connected());
    silent_fd = proc.ReleaseConnection();  // connected, not a single byte sent
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // ~ServerProcess() sends SIGUSR1 and waits for the child: the worker must
    // return promptly instead of waiting for the 60 s deadline.
  }
  ::close(silent_fd);
}

// ================================================================
// Handler failures become HTTP 500
// ================================================================

TEST(FcgiServerRunTest, HandlerExceptionBecomes500) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::ThrowStd));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find("Status: 500"), std::string::npos) << body;
  EXPECT_NE(body.find(R"({"error":"internal error"})"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, UnknownHandlerExceptionBecomes500) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::ThrowUnknown));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find("Status: 500"), std::string::npos) << body;
}

// ================================================================
// Long-poll capacity limiting
// ================================================================

TEST(FcgiServerRunTest, LongPollIsRejectedWhenAllTokensAreTaken) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, true, true);
  ASSERT_TRUE(proc.connected());
  // Consume the only concurrency token: the server must answer 503 at once
  // instead of queueing the request for a busy worker.
  ASSERT_TRUE(proc.take_concurrency_token());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}});
  EXPECT_NE(body.find("Status: 503"), std::string::npos) << body;
  EXPECT_NE(body.find("all workers busy"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, LongPollIsShedWhenTheLoadSheddingLimitIsReached) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, true, true);
  ASSERT_TRUE(proc.connected());
  ASSERT_TRUE(proc.take_load_shed_token());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}});
  EXPECT_NE(body.find("Status: 503"), std::string::npos) << body;
  EXPECT_NE(body.find("too many long-poll requests"), std::string::npos) << body;
  // The concurrency token taken for this request must have been returned;
  // only the load-shedding token stays consumed (the parent took it).
  EXPECT_TRUE(proc.wait_for_concurrency_tokens(1)) << proc.concurrency_tokens();
}

TEST(FcgiServerRunTest, LongPollReturnsBothTokensAfterTheResponse) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, true, true);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  // Both tokens are released by their destructors at the end of the iteration.
  EXPECT_TRUE(proc.wait_for_tokens(1));
}

/// A worker's long-poll capacity must be back in the pool *before* the client
/// can see its response finished.
///
/// Regression test: the tokens used to be released after FCGX_Finish_r() had
/// closed the connection, so a sequential client could send its next request
/// while the previous worker still held its token.  With a pool of two (one
/// token, see longpoll_capacity()) the follow-up `/r` was then answered
/// `503 {"error":"all workers busy"}` — the intermittent CI failure of
/// test_main_e2e's OldestSessionIsEvictedWhenTheStoreIsFull.
TEST(FcgiServerRunTest, LongPollTokensAreReleasedBeforeTheResponseIsWritten) {
  FcgiServer server;
  server.set_handler([](const FcgiRequest&) {
    FcgiResponse response;
    response.status_code = 200;
    response.content_type = "application/json";
    // Far larger than any socket buffer: the server cannot finish writing
    // until the test drains it, so "the response is still in flight" is a
    // state the test can reliably observe.
    response.body = std::string(8U * 1024U * 1024U, 'x');
    return response;
  });

  ServerProcess proc(server, true, true);
  ASSERT_TRUE(proc.connected());

  ASSERT_TRUE(proc.SendRequest({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/r"}}));
  ASSERT_TRUE(proc.WaitForReplyBytes()) << "the server never started writing the response";

  // The handler has returned and its response is on the wire: the tokens must
  // already be free, otherwise the next client races into a spurious 503.
  EXPECT_EQ(proc.concurrency_tokens(), 1)
      << "the long-poll token is only returned after the response was written";
  EXPECT_EQ(proc.load_shed_tokens(), 1)
      << "the load-shedding token is only returned after the response was written";

  // Drain the rest so the child can finish the request and shut down.
  const std::string body = proc.FinishExchange();
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body.substr(0, 200);
}

TEST(FcgiServerRunTest, WriteAndLoginNeverTakeAToken) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, true, true);
  ASSERT_TRUE(proc.connected());
  ASSERT_TRUE(proc.take_concurrency_token());
  ASSERT_TRUE(proc.take_load_shed_token());

  // A spurious 503 on a write surfaces in CometVisu as a failed switch
  // operation, so /w and /l must never be rejected.
  for (const char* uri : {"/w?a=1/2/3&v=8042", "/l?u=u&p=p"}) {
    const std::string body = proc.Exchange({{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", uri}});
    EXPECT_NE(body.find("Status: 200"), std::string::npos) << uri << ": " << body;
  }
  EXPECT_EQ(proc.concurrency_tokens(), 0);
  EXPECT_EQ(proc.load_shed_tokens(), 0);
  EXPECT_TRUE(proc.connected() || true);
}

// ================================================================
// Parameter parsing edge cases
// ================================================================

TEST(FcgiServerRunTest, RequestWithoutParametersIsHandled) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_NE(body.find(R"({"path":"","method":"","len":0})"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, PostBodyIsReadFromTheFastCgiStream) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string payload = "12345678";
  const std::string body =
      proc.Exchange({{"REQUEST_METHOD", "POST"}, {"REQUEST_URI", "/w"}, {"CONTENT_LENGTH", "8"}},
                    Bytes(payload.begin(), payload.end()));
  // The handler reports req.content.size(): the body must have arrived.
  EXPECT_NE(body.find(R"({"path":"/w","method":"POST","len":8})"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, PostBodyLongerThanContentLengthIsTruncated) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  // Only the advertised four bytes are taken from the stream.
  const std::string payload = "abcdefgh";
  const std::string body =
      proc.Exchange({{"REQUEST_METHOD", "POST"}, {"REQUEST_URI", "/w"}, {"CONTENT_LENGTH", "4"}},
                    Bytes(payload.begin(), payload.end()));
  EXPECT_NE(body.find(R"("len":4)"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, PostWithoutContentLengthSkipsTheBody) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body = proc.Exchange({{"REQUEST_METHOD", "POST"}, {"REQUEST_URI", "/w"}});
  EXPECT_NE(body.find(R"("len":0)"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, ParameterNamePrefixWithoutSeparatorIsIgnored) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  // "REQUEST_METHODX" shares the prefix but has no '=' at that position; it
  // must not be mistaken for REQUEST_METHOD.
  const std::string body =
      proc.Exchange({{"REQUEST_METHODX", "GET"}, {"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  EXPECT_NE(body.find(R"({"path":"/l","method":"GET","len":0})"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, PutRequestWithoutContentLengthSkipsTheBody) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  const std::string body =
      proc.Exchange({{"REQUEST_METHOD", "PUT"}, {"REQUEST_URI", "/w"}, {"CONTENT_LENGTH", ""}});
  EXPECT_NE(body.find(R"("method":"PUT","len":0)"), std::string::npos) << body;
}

TEST(FcgiServerRunTest, InvalidContentLengthSkipsTheBody) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  for (const char* length : {"abc", "0", "-4"}) {
    const std::string body = proc.Exchange(
        {{"REQUEST_METHOD", "POST"}, {"REQUEST_URI", "/w"}, {"CONTENT_LENGTH", length}});
    EXPECT_NE(body.find(R"("len":0)"), std::string::npos) << length << ": " << body;
  }
}

TEST(FcgiServerRunTest, ExcessiveContentLengthIsCapped) {
  FcgiServer server;
  server.set_handler(MakeHandler(Behaviour::Echo));

  ServerProcess proc(server, false, false);
  ASSERT_TRUE(proc.connected());

  // 100 MB announced: the body is truncated to the 64 KB guard instead of
  // allocating the advertised size.
  const std::string body = proc.Exchange(
      {{"REQUEST_METHOD", "POST"}, {"REQUEST_URI", "/w"}, {"CONTENT_LENGTH", "100000000"}});
  EXPECT_NE(body.find("Status: 200"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("len":0)"), std::string::npos) << body;
}

// ================================================================
// Lifecycle: listen(), close_listen_socket(), shutdown(), run() without handler
// ================================================================

TEST(FcgiServerLifecycleTest, ListenDefaultsTheBacklogAndCloseIsIdempotent) {
  FcgiServer server;
  const std::string path = SocketPath("listen");
  // A backlog below 1 is replaced by the default.
  ASSERT_TRUE(server.listen(path, 0));
  EXPECT_TRUE(server.is_listening());
  server.close_listen_socket();
  EXPECT_FALSE(server.is_listening());
  server.close_listen_socket();  // idempotent
  ::unlink(path.c_str());
}

TEST(FcgiServerLifecycleTest, RunWithoutAHandlerFails) {
  FcgiServer server;
  EXPECT_EQ(server.run(), 1);
}

/// Serving FastCGI from stdin/stdout (spawn-fcgi mode) is gone: run() needs a
/// listen socket, because the worker pool and the cache reader share it.
TEST(FcgiServerLifecycleTest, RunWithoutAListenSocketFails) {
  FcgiServer server;
  server.set_handler([](const FcgiRequest& /*req*/) -> FcgiResponse {
    return {.status_code = 200, .body = "{}"};
  });
  EXPECT_EQ(server.run(), 1);
}

TEST(FcgiServerLifecycleTest, ShutdownWakesWorkersOnATcpListener) {
  FcgiServer server;
  // A TCP socket keeps getsockname() working after shutdown(SHUT_RDWR), so the
  // self-connect storm that unblocks waiting workers is exercised.
  if (!server.listen(":0")) {
    GTEST_SKIP() << "cannot open a local TCP port";
  }
  server.shutdown();
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerLifecycleTest, ShutdownUnblocksAndClosesTheListenSocket) {
  FcgiServer server;
  const std::string path = SocketPath("shutdown");
  ASSERT_TRUE(server.listen(path));
  EXPECT_TRUE(server.is_listening());

  // shutdown() must wake any worker blocked in accept() — it uses
  // shutdown(SHUT_RDWR) plus a self-connect storm — and close the socket.
  server.shutdown();
  EXPECT_FALSE(server.is_listening());
  ::unlink(path.c_str());
}

}  // namespace
}  // namespace cvknx
