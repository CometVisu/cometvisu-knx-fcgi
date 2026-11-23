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

#include <fcgiapp.h>
#include <gtest/gtest.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fcgi/fcgi_request.h"
#include "fcgi/fcgi_server.h"

using namespace cvknx;

namespace {

/// Generate a unique socket path in the writable temp directory.
/// Returns empty string on failure.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || *tmpdir == '\0') {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/fcgi-fork-integration-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) {
    return "";
  }
  close(fd);
  unlink(buf.data());
  return std::string(buf.data()) + ".sock";
}

/// Check if we can create Unix sockets. The VS Code sandbox may block
/// AF_UNIX socket creation, in which case we skip these tests.
bool sockets_available() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  close(fd);

  // Also verify we can bind
  std::string path = make_unique_socket_path();
  if (path.empty()) {
    return false;
  }
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    return false;
  }
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[path_len] = '\0';
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  int rc = bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  close(s);
  unlink(path.c_str());
  return rc == 0;
}

/// Wrapper to connect to a Unix socket with retries (children may not
/// be ready immediately after fork).
int connect_with_retry(const std::string& path, int max_retries, int delay_ms) {
  for (int i = 0; i < max_retries; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
      return fd;
    }

    close(fd);
    usleep(static_cast<useconds_t>(delay_ms) * 1000);
  }
  return -1;
}

/// Signature of the fork primitive used by fork_workers().
///
/// It is injectable so the fork-failure path can be tested deterministically.
/// Lowering RLIMIT_NPROC (as this test used to do) is not reliable: the limit
/// counts the tasks of the whole real UID, so whether a few forks succeed
/// before the failure depends on whatever else runs as that user — a handful
/// of processes on a developer machine, thousands in a desktop session.
using ForkFn = std::function<pid_t()>;

/// SIGKILL and reap the given children.
///
/// SIGTERM must not be used for workers blocked in the FCGI accept loop:
/// FCGX_Accept_r() keeps retrying on EINTR, so a worker may well ignore it and
/// a blocking wait() would then hang forever.  SIGKILL cannot be caught or
/// blocked.  The signal is delivered before kill() returns for a runnable
/// target, so the per-pid waitpid() cannot block for long.
void kill_children(const std::vector<pid_t>& children) {
  for (pid_t pid : children) {
    ::kill(pid, SIGKILL);
  }
  for (pid_t pid : children) {
    int ws = 0;
    (void)::waitpid(pid, &ws, 0);
  }
}

/// Fork N children, each running an accept loop on the
/// shared listen socket. Returns child PIDs on success.
/// On failure in the parent, kills started children and returns empty vector.
std::vector<pid_t> fork_workers(int num_workers, FcgiServer& server, const char* knxd_socket,
                                ForkFn fork_fn = ::fork) {
  std::vector<pid_t> children;
  children.reserve(static_cast<size_t>(num_workers));
  const pid_t parent_pid = ::getpid();

  for (int i = 0; i < num_workers; ++i) {
    const pid_t pid = fork_fn();
    if (pid < 0) {
      // Fork failed — kill and reap all started children
      std::cerr << "[TEST-ERROR] fork() for worker " << (i + 1) << "/" << num_workers
                << " failed: " << std::strerror(errno) << " (errno=" << errno << ")\n";
      kill_children(children);
      children.clear();
      break;
    }

    if (pid == 0) {
      // ---- Child process ----
      // A worker must never outlive the test process: a leaked worker keeps
      // the pipe open that ctest uses to capture this test's output, so ctest
      // waits for an EOF that never comes and the CI job hangs at
      // "Start N: test_fork_workers" instead of reporting a result.
      (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
      // Close the race where the parent died before prctl() took effect.
      if (::getppid() != parent_pid) {
        std::_Exit(1);
      }

      // Reconnect to knxd if a socket path was provided.
      // In the test, knxd_socket may be nullptr (no knxd needed
      // for pure FCGI socket tests).
      (void)knxd_socket;  // silence unused warning in test

      // Run accept loop in this child process.
      // The listen socket is shared with siblings via fork().
      int child_result = server.run();
      std::_Exit(child_result);
    }

    children.push_back(pid);
  }

  return children;
}

}  // namespace

// ============================================================
// ForkWorkerTest — verifies the fork-based concurrency pattern
// ============================================================
//
// These tests validate that forking N children, each running
// FcgiServer::run() on a shared listen socket, correctly handles
// concurrent clients. This is the pattern used in main.cpp when
// std::thread / pthread_create is blocked by seccomp (Docker < 20.10.10
// with glibc >= 2.34).
//
class ForkWorkerTest : public ::testing::Test {
public:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation is blocked by sandbox";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
  }

  void TearDown() override {
    // Reap every worker this test started — also when the test body returned
    // early through a failed ASSERT.  A leaked worker would otherwise keep
    // ctest's output pipe open and hang the whole test run.
    kill_children(children_);
    children_.clear();
    unlink(socket_path_.c_str());
  }

  /// Fork workers and remember them so that TearDown() can always reap them.
  std::vector<pid_t> start_workers(int num_workers, FcgiServer& server, ForkFn fork_fn = ::fork) {
    auto children = fork_workers(num_workers, server, nullptr, fork_fn);
    children_.insert(children_.end(), children.begin(), children.end());
    return children;
  }

  std::string socket_path_;
  /// All children started by the current test (reaped in TearDown()).
  std::vector<pid_t> children_;
};

// ------------------------------------------------------------------
// Test: Forked workers accept connections on the shared listen socket
//
// Verifies that forked children, each running FcgiServer::run() on
// the same listen socket, can accept plain socket connections.
// This proves that the fork+accept pattern works without needing a
// full FCGI protocol exchange.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, ForkedWorkersAcceptConnections) {
  constexpr int kNumWorkers = 2;

  FcgiServer server;
  // Set a handler — not strictly needed for connection tests, but
  // validates that the handler callback is captured correctly.
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"status\":\"ok\"}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  // Fork workers
  auto children = start_workers(kNumWorkers, server);
  ASSERT_EQ(children.size(), static_cast<size_t>(kNumWorkers))
      << "fork_workers failed to create all " << kNumWorkers << " children";

  // Verify all children are alive
  for (size_t i = 0; i < children.size(); ++i) {
    ASSERT_EQ(::kill(children[i], 0), 0)
        << "Child " << i << " (pid=" << children[i] << ") not alive after fork";
  }

  // Connect multiple times — each connection should be accepted by one
  // of the forked workers. The OS serializes accept() across processes.
  //
  // Send valid minimal FCGI frames (BEGIN_REQUEST + empty PARAMS) instead
  // of plain data.  Sending raw bytes like "ping" causes FCGX_Accept_r()
  // to enter an internal error-handling loop that may hang indefinitely
  // depending on the compiler's code generation and libfcgi version.
  constexpr int kConnections = 4;
  for (int i = 0; i < kConnections; ++i) {
    int fd = connect_with_retry(socket_path_, 10, 50);
    ASSERT_GE(fd, 0) << "Failed to connect to listen socket on attempt " << i << " of "
                     << kConnections;

    // Send a minimal valid FCGI request: BEGIN_REQUEST + empty PARAMS
    static constexpr std::array<unsigned char, 16> kBeginRequest = {
        // FCGI header — BEGIN_REQUEST
        1,     // version = FCGI_VERSION_1
        1,     // type = FCGI_BEGIN_REQUEST
        0, 1,  // requestId = 1
        0, 8,  // contentLength = 8
        0,     // paddingLength
        0,     // reserved
        // FCGI_BeginRequestBody
        0, 1,          // role = FCGI_RESPONDER
        0,             // flags
        0, 0, 0, 0, 0  // reserved[5]
    };
    static constexpr std::array<unsigned char, 8> kEmptyParams = {
        // FCGI header — PARAMS (empty = end of params)
        1,     // version = FCGI_VERSION_1
        4,     // type = FCGI_PARAMS
        0, 1,  // requestId = 1
        0, 0,  // contentLength = 0
        0,     // paddingLength
        0      // reserved
    };
    static constexpr std::array<unsigned char, 8> kEmptyStdin = {
        // FCGI header — STDIN (empty = end of stdin)
        1,     // version = FCGI_VERSION_1
        5,     // type = FCGI_STDIN
        0, 1,  // requestId = 1
        0, 0,  // contentLength = 0
        0,     // paddingLength
        0      // reserved
    };
    ssize_t begin_written = ::write(fd, kBeginRequest.data(), kBeginRequest.size());
    (void)begin_written;
    ssize_t params_written = ::write(fd, kEmptyParams.data(), kEmptyParams.size());
    (void)params_written;
    ssize_t stdin_written = ::write(fd, kEmptyStdin.data(), kEmptyStdin.size());
    (void)stdin_written;

    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }

  // All children should still be alive after handling connections
  for (size_t i = 0; i < children.size(); ++i) {
    EXPECT_EQ(::kill(children[i], 0), 0)
        << "Child " << i << " (pid=" << children[i] << ") died unexpectedly";
  }

  // TearDown() kills and reaps the workers.
}

// ------------------------------------------------------------------
// Test: Fork worker error handling — fork failure is reported
//
// Tests the error path in the fork loop: when fork() fails, the
// already-started children are killed and reaped, and the caller gets an
// empty vector to indicate failure.
//
// The fork primitive is injected so the failure point is deterministic.  The
// previous version lowered RLIMIT_NPROC to force the failure, which depends
// on how many tasks the whole real UID already has — on a CI runner a few
// forks still succeeded, and the error path could then block in wait().
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, PartialForkFailureIsReportedAndCleanedUp) {
  FcgiServer server;
  // Set handler to avoid "No request handler set" stderr noise in children
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.body = "{}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  // Fail the fork loop after two successfully started workers.
  std::vector<pid_t> started;
  int calls = 0;
  auto children = start_workers(5, server, [&calls, &started]() -> pid_t {
    if (calls++ >= 2) {
      errno = EAGAIN;  // simulate resource exhaustion
      return -1;
    }
    const pid_t pid = ::fork();
    if (pid > 0) {
      started.push_back(pid);
    }
    return pid;
  });

  // Verify the function didn't hang or crash and reports the failure.
  EXPECT_TRUE(children.empty()) << "fork_workers() must report a failed fork";
  ASSERT_EQ(started.size(), static_cast<size_t>(2))
      << "expected two workers before the injected fork failure";

  // The already-started workers must have been killed and reaped.
  for (pid_t pid : started) {
    EXPECT_EQ(::kill(pid, 0), -1) << "worker " << pid << " was leaked by fork_workers()";
    EXPECT_EQ(errno, ESRCH) << "worker " << pid << " is still alive";
  }
}

// ------------------------------------------------------------------
// Test: worker children never outlive their parent
//
// A worker that survives the test process keeps the pipe open that ctest uses
// to capture the test output.  ctest then waits forever for an EOF that never
// comes and the CI job hangs at "Start N: test_fork_workers" instead of
// reporting a result.  PR_SET_PDEATHSIG in fork_workers() guarantees that the
// workers are killed together with their parent.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, WorkersDieWithTheirParent) {
  FcgiServer server;
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.body = "{}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  // The intermediate process forks the workers and reports their PIDs over a
  // pipe; the test process then SIGKILLs that intermediate process.
  std::array<int, 2> pipe_fds{-1, -1};
  ASSERT_EQ(::pipe(pipe_fds.data()), 0) << "pipe() failed: " << std::strerror(errno);

  const pid_t middle = ::fork();
  ASSERT_GE(middle, 0) << "fork() failed: " << std::strerror(errno);

  if (middle == 0) {
    // ---- intermediate process ----
    ::close(pipe_fds[0]);
    auto workers = fork_workers(2, server, nullptr);
    const size_t bytes = workers.size() * sizeof(pid_t);
    (void)!::write(pipe_fds[1], workers.data(), bytes);
    ::close(pipe_fds[1]);
    for (;;) {
      ::pause();  // killed by the test process below
    }
  }

  children_.push_back(middle);  // let TearDown() clean up on early return
  ::close(pipe_fds[1]);

  std::array<pid_t, 2> worker_pids{};
  size_t received = 0;
  while (received < sizeof(worker_pids)) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const ssize_t n = ::read(pipe_fds[0], reinterpret_cast<char*>(worker_pids.data()) + received,
                             sizeof(worker_pids) - received);
    if (n <= 0) {
      break;
    }
    received += static_cast<size_t>(n);
  }
  ::close(pipe_fds[0]);
  ASSERT_EQ(received, sizeof(worker_pids)) << "intermediate process did not report its workers";

  // Sanity check: the workers are alive while their parent is alive.
  for (pid_t pid : worker_pids) {
    ASSERT_EQ(::kill(pid, 0), 0) << "worker " << pid << " did not start";
  }

  // Kill the parent — its workers have to follow.
  ASSERT_EQ(::kill(middle, SIGKILL), 0);
  int ws = 0;
  ASSERT_EQ(::waitpid(middle, &ws, 0), middle);

  for (pid_t pid : worker_pids) {
    bool gone = false;
    for (int attempt = 0; attempt < 100 && !gone; ++attempt) {  // up to 2 s
      if (::kill(pid, 0) != 0 && errno == ESRCH) {
        gone = true;
      } else {
        usleep(20 * 1000);
      }
    }
    EXPECT_TRUE(gone) << "worker " << pid
                      << " outlived its parent (an orphan keeps ctest's pipe open)";
    ::kill(pid, SIGKILL);  // best-effort cleanup if the expectation failed
  }
}

// ------------------------------------------------------------------
// Test: Child process exits cleanly when parent sends SIGTERM
//
// Verifies that a forked worker process running FcgiServer::run()
// exits when killed by the parent. This is the normal shutdown path
// in production.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, ChildExitsOnSigterm) {
  FcgiServer server;
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.body = "{}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  auto children = start_workers(1, server);
  ASSERT_EQ(children.size(), 1U) << "Failed to fork single worker";

  pid_t child_pid = children[0];

  // Give child time to initialize FCGX and enter accept()
  usleep(100000);

  // Verify child is alive
  int result = ::kill(child_pid, 0);
  ASSERT_EQ(result, 0) << "Child pid=" << child_pid << " is not alive before SIGTERM";

  // Send SIGTERM
  ::kill(child_pid, SIGTERM);
  usleep(100000);

  // Child should have exited by now
  int status = 0;
  pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
  if (waited == 0) {
    // Still running — force kill
    ::kill(child_pid, SIGKILL);
    waited = ::waitpid(child_pid, &status, 0);
  }
  EXPECT_EQ(waited, child_pid) << "Child pid=" << child_pid << " did not exit after SIGTERM";
  EXPECT_TRUE(WIFSIGNALED(status) || WIFEXITED(status))
      << "Child neither exited nor was signaled (status=" << status << ")";
}
