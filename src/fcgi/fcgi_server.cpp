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
 * @file fcgi_server.cpp
 * @brief FastCGI server implementation — accept loop, request parsing, response writing.
 *
 * The server owns a listen socket (created from FCGI_SOCKET, a Unix path or a
 * TCP port) and uses the FCGX API, so the fork-based worker pool can share the
 * socket: each child calls FCGX_Accept_r() on the inherited fd and the kernel
 * serialises the accepts.  Serving FastCGI from stdin/stdout (the classic
 * spawn-fcgi model) is deliberately not supported: the architecture requires a
 * dedicated cache-reader process that owns the single bus connection, and that
 * process only exists in the socket mode (see AGENTS.md).
 */

#include "fcgi_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../util/debug_log.h"

namespace cvknx {

/// Maximum allowed request body size (64 KB).
/// Prevents OOM from a malicious CONTENT_LENGTH.
inline constexpr int kMaxContentLength = 64 * 1024;

namespace {

/// Releases a semaphore token on scope exit.
///
/// The request loop has several exit paths (early `continue`, a throwing
/// handler, an assertion failure in the handler) — all of them must return
/// the token.  A leaked token permanently reduces the capacity of the whole
/// worker pool, because nothing ever restores it.
class SemToken {  // NOLINT(cppcoreguidelines-special-member-functions)
public:
  SemToken() = default;
  explicit SemToken(sem_t* sem) : sem_(sem) {}
  ~SemToken() { release(); }

  SemToken(const SemToken&) = delete;
  SemToken& operator=(const SemToken&) = delete;
  SemToken(SemToken&& other) noexcept : sem_(other.sem_) { other.sem_ = nullptr; }
  SemToken& operator=(SemToken&& other) noexcept {
    if (this != &other) {  // GCOV_EXCL_BR_LINE - self-move-assignment never happens
      release();
      sem_ = other.sem_;
      other.sem_ = nullptr;
    }
    return *this;
  }

  void release() {
    if (sem_ != nullptr) {
      ::sem_post(sem_);
      sem_ = nullptr;
    }
  }

private:
  sem_t* sem_ = nullptr;
};

}  // namespace

FcgiServer::FcgiServer() = default;

FcgiServer::~FcgiServer() {
  shutdown();
}

void FcgiServer::set_handler(RequestHandler handler) {
  handler_ = std::move(handler);
}

void FcgiServer::set_load_shed_semaphore(sem_t* sem) {
  load_shed_sem_ = sem;
}

void FcgiServer::set_concurrency_semaphore(sem_t* sem) {
  concurrency_sem_ = sem;
}

FcgiResponse FcgiServer::invoke_handler(const FcgiRequest& request) {
  try {
    return handler_(request);
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] Request handler threw an exception: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "[ERROR] Request handler threw an unknown exception\n";
  }

  FcgiResponse resp;
  resp.status_code = 500;
  resp.body = R"({"error":"internal error"})";
  return resp;
}

bool FcgiServer::listen(const std::string& socket_path, int backlog) {
  if (socket_path.empty()) {
    return false;
  }
  if (backlog < 1) {
    backlog = 128;
  }
  const int fd = FCGX_OpenSocket(socket_path.c_str(), backlog);
  // GCOV_EXCL_START - this libfcgi terminates the process on a bind failure
  // (exit(2)), so a negative return cannot be provoked from a test.
  if (fd < 0) {
    return false;
  }
  // GCOV_EXCL_STOP
  listen_fd_ = fd;
  return true;
}

bool FcgiServer::is_listening() const {
  return listen_fd_ >= 0;
}

void FcgiServer::close_listen_socket() {
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

int FcgiServer::run() {
  if (!handler_) {
    std::cerr << "[ERROR] No request handler set\n";
    return 1;
  }

  if (listen_fd_ < 0) {
    std::cerr << "[ERROR] No FastCGI listen socket — set FCGI_SOCKET to a Unix socket path or a "
                 "TCP port\n";
    return 1;
  }

  // FCGX_Init() must be called before FCGX_Accept_r() — it sets the
  // libInitialized flag and performs platform-specific setup (OS_LibInit).
  // FCGX_InitRequest() alone does NOT mark the library as initialized.
  // GCOV_EXCL_START - FCGX_Init() only fails on internal resource
  // exhaustion; repeated calls return 0 as well (verified), so no test can
  // reach this branch.
  if (FCGX_Init() != 0) {
    std::cerr << "[ERROR] FCGX_Init failed\n";
    return 1;
  }
  // GCOV_EXCL_STOP

  // GCOV_EXCL_START - see above: FCGX_InitRequest() returns 0 for a valid fd
  // and cannot be made to fail from a test.
  if (FCGX_InitRequest(&request_, listen_fd_, 0) != 0) {
    std::cerr << "[ERROR] FCGX_InitRequest failed\n";
    return 1;
  }
  // GCOV_EXCL_STOP

  // Accept FastCGI connections on our own Unix/TCP socket.  Each worker of
  // the fork-based pool inherits listen_fd_ and calls FCGX_Accept_r()
  // independently; the kernel serialises the accept calls.
  while (FCGX_Accept_r(&request_) >= 0) {
    // Read the request parameters from the FCGI parameter array.  The
    // spawn-fcgi-era getenv() variant is gone: envp is passed explicitly,
    // which is thread-safe and avoids mutating the global `environ`.
    const FcgiRequest req = read_request(request_.envp);

    // ---- Capacity limiting for long-poll reads only ----
    // Only /r can hold a worker for hundreds of seconds, so only /r takes a
    // token.  Writes (/w) and logins (/l) finish in microseconds and must
    // never be rejected — a spurious 503 here surfaces in CometVisu as a
    // failed switch operation.
    //
    // The tokens are released by their destructors at the end of this
    // iteration, so every exit path (early `continue`, throwing handler)
    // returns the capacity.  A leaked token permanently shrinks the pool.
    SemToken concurrency_token;
    SemToken load_shed_token;
    if (req.path() == "/r") {
      if (concurrency_sem_ != nullptr) {
        if (::sem_trywait(concurrency_sem_) != 0) {
          // All workers busy — return 503 immediately
          FcgiResponse busy_resp;
          busy_resp.status_code = 503;
          busy_resp.body = R"({"error":"all workers busy"})";
          write_response(busy_resp);
          FCGX_Finish_r(&request_);
          continue;
        }
        concurrency_token = SemToken(concurrency_sem_);
      }
      if (load_shed_sem_ != nullptr) {
        // sem_trywait returns 0 on success, -1 with EAGAIN if exhausted
        if (::sem_trywait(load_shed_sem_) != 0) {
          // All workers busy with long-polls — return empty immediately.
          // The client will retry, and this frees up the accept queue for
          // write requests which complete quickly.
          FcgiResponse empty_resp;
          empty_resp.status_code = 503;
          empty_resp.body = R"({"error":"too many long-poll requests"})";
          write_response(empty_resp);
          FCGX_Finish_r(&request_);
          continue;
        }
        load_shed_token = SemToken(load_shed_sem_);
      }
    }

    DebugLog::http_request(req.request_method, req.request_uri);

    const FcgiResponse resp = invoke_handler(req);

    DebugLog::http_response(resp.status_code, resp.body);

    write_response(resp);

    FCGX_Finish_r(&request_);
  }

  return 0;
}

void FcgiServer::shutdown() {
  shutdown_requested_.store(true, std::memory_order_relaxed);

  if (listen_fd_ >= 0) {
    // Three-step shutdown to reliably unblock workers stuck in accept():
    //
    // Step 1: shutdown(SHUT_RDWR) causes any worker blocked in accept()
    // on this fd to fail with EINVAL.  This is reliable on Linux, unlike
    // close() which may leave accept() blocked indefinitely because the
    // kernel keeps the fd alive while children hold references.
    ::shutdown(listen_fd_, SHUT_RDWR);

    // Step 2: Self-connect to unblock any workers that didn't respond to
    // shutdown().  Each accepted connection gets an immediate EOF (we
    // close our end right away), causing FCGX_Accept_r() to return -1
    // and the worker to exit its loop naturally.
    //
    // Connecting 64 times is a belt-and-suspenders approach — it covers
    // the pathological case where multiple workers were stuck in accept()
    // simultaneously and the kernel delivered the shutdown signal to only
    // one of them.
    // GCOV_EXCL_START - on Linux getsockname() on a listening socket fails
    // with ENOTCONN once shutdown(SHUT_RDWR) ran, so the storm below never
    // executes here; it exists for platforms that keep the socket name.
    struct sockaddr_un addr {};
    socklen_t addr_len = sizeof(addr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
      const int wakeups = 64;
      for (int i = 0; i < wakeups; ++i) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) {
          break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
        if (ret == 0 || errno == EINPROGRESS) {
          // Connection queued — one worker will accept it
        } else {
          ::close(fd);
          break;
        }
        ::close(fd);
      }
    }
    // GCOV_EXCL_STOP

    // Step 3: Close the fd.  By now all workers should have exited.
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

FcgiRequest FcgiServer::read_request(
    char** envp) {  // NOLINT(readability-convert-member-functions-to-static)
  FcgiRequest req;

  // GCOV_EXCL_START - FCGX_Accept_r() always provides a parameter array.
  if (envp == nullptr) {
    return req;
  }
  // GCOV_EXCL_STOP

  // Helper: find a value by name in a null-terminated envp array.
  // Each entry is "KEY=VALUE". Returns pointer to VALUE part, or nullptr.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto get_env = [](char** env_array, const char* name) -> const char* {
    // GCOV_EXCL_START - the caller checks envp and passes literal names.
    if (env_array == nullptr || name == nullptr) {
      return nullptr;
    }
    // GCOV_EXCL_STOP
    const size_t name_len = std::strlen(name);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (const char* const* e = const_cast<const char* const*>(env_array); *e != nullptr; ++e) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (std::strncmp(*e, name, name_len) == 0 && (*e)[name_len] == '=') {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return *e + name_len + 1;
      }
    }
    return nullptr;
  };

  auto get_env_or_empty = [&](const char* name) -> const char* {
    const char* val = get_env(envp, name);
    return val != nullptr ? val : "";
  };

  req.request_method = get_env_or_empty("REQUEST_METHOD");
  req.request_uri = get_env_or_empty("REQUEST_URI");
  req.query_string = get_env_or_empty("QUERY_STRING");
  req.content_type = get_env_or_empty("CONTENT_TYPE");
  req.script_name = get_env_or_empty("SCRIPT_NAME");
  req.path_info = get_env_or_empty("PATH_INFO");
  req.server_protocol = get_env_or_empty("SERVER_PROTOCOL");

  // Read request body (for POST data, e.g., filter operations)
  if (req.request_method == "POST" || req.request_method == "PUT") {
    const char* content_length_str = get_env(envp, "CONTENT_LENGTH");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (content_length_str != nullptr && content_length_str[0] != '\0') {
      int content_length = 0;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const char* cl_end = content_length_str + std::strlen(content_length_str);
      auto [ptr, ec] = std::from_chars(content_length_str, cl_end, content_length);
      if (ec != std::errc{} || content_length <= 0) {
        return req;
      }
      if (content_length > kMaxContentLength) {
        std::cerr << "[WARN] CONTENT_LENGTH " << content_length << " exceeds maximum "
                  << kMaxContentLength << ", truncating\n";
        content_length = kMaxContentLength;
      }
      const auto max_len = static_cast<size_t>(content_length);
      req.content.resize(max_len);
      size_t total = 0;
      while (total < max_len) {
        const size_t remaining = max_len - total;
        // FCGX_GetStr() reads from this request's stdin stream (`request_.in`)
        // and returns at most `remaining` bytes, 0 at the end of the body and
        // -1 on error.  The count is clamped so that `total` can never exceed
        // the buffer: an oversized value would make resize() throw out of run()
        // and kill the worker.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const int got = FCGX_GetStr(reinterpret_cast<char*>(req.content.data() + total),
                                    static_cast<int>(remaining), request_.in);
        if (got <= 0) {
          break;  // end of body (or a read error) — keep what arrived
        }
        total += std::min(static_cast<size_t>(got), remaining);
      }
      req.content.resize(total);
    }
  }

  return req;
}

void FcgiServer::write_response(const FcgiResponse& response) {
  // FastCGI has no status line in its protocol: the HTTP status travels as a
  // `Status:` header and Content-Length must be set explicitly, because libfcgi
  // streams the headers and the body through unchanged.
  std::string output;
  output.reserve(256 + response.body.size());

  output += "Status: " + std::to_string(response.status_code) + "\r\n";
  output += "Content-Type: " + response.content_type + "; charset=utf-8\r\n";
  output += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  output += "\r\n";
  output += response.body;

  FCGX_PutStr(output.data(), static_cast<int>(output.size()), request_.out);
}

}  // namespace cvknx
