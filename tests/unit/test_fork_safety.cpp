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
 * @file test_fork_safety.cpp
 * @brief flush_before_fork() keeps a forked child from re-emitting the
 *        parent's buffered log lines.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "util/fork_safety.h"

namespace {

/// Redirect STDOUT_FILENO into a pipe and collect everything written to it.
class StdoutCapture {
public:
  StdoutCapture() {
    saved_fd_ = ::dup(STDOUT_FILENO);
    int fds[2] = {-1, -1};
    if (saved_fd_ < 0 || ::pipe(fds) != 0) {
      return;
    }
    if (::dup2(fds[1], STDOUT_FILENO) < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      return;
    }
    // STDOUT_FILENO is now the only descriptor holding the write end.
    ::close(fds[1]);
    read_fd_ = fds[0];
    active_ = true;
  }

  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;
  StdoutCapture(StdoutCapture&&) = delete;
  StdoutCapture& operator=(StdoutCapture&&) = delete;

  ~StdoutCapture() { (void)stop_and_read(); }

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] int read_fd() const { return read_fd_; }

  /// Restore STDOUT_FILENO, then drain the pipe until EOF.
  std::string stop_and_read() {
    if (!active_) {
      return output_;
    }
    std::cout.flush();
    if (saved_fd_ >= 0) {
      (void)::dup2(saved_fd_, STDOUT_FILENO);
      ::close(saved_fd_);
      saved_fd_ = -1;
    }
    active_ = false;
    char buf[1024];
    for (;;) {
      const ssize_t n = ::read(read_fd_, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      output_.append(buf, static_cast<size_t>(n));
    }
    ::close(read_fd_);
    read_fd_ = -1;
    return output_;
  }

private:
  int saved_fd_ = -1;
  int read_fd_ = -1;
  bool active_ = false;
  std::string output_;
};

/// Number of non-overlapping occurrences of @p needle in @p haystack.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

/// Wait until @p fd has data or @p timeout_ms elapsed.
bool wait_for_data(int fd, int timeout_ms) {
  struct pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  return ::poll(&pfd, 1, timeout_ms) > 0;
}

TEST(ForkSafetyTest, FlushBeforeForkWritesPendingLinesImmediately) {
  StdoutCapture capture;
  ASSERT_TRUE(capture.active()) << "cannot redirect stdout";

  std::cout << "pending-line\n";  // intentionally left in the buffer
  cvknx::flush_before_fork();

  EXPECT_TRUE(wait_for_data(capture.read_fd(), 1000))
      << "flush_before_fork() left data in the stdio buffer";

  const std::string out = capture.stop_and_read();
  EXPECT_EQ(1U, count_occurrences(out, "pending-line")) << "captured output:\n" << out;
}

TEST(ForkSafetyTest, ForkedChildDoesNotRepeatTheParentsBufferedLines) {
  StdoutCapture capture;
  ASSERT_TRUE(capture.active()) << "cannot redirect stdout";

  // The parent logs, the child logs: each line must appear exactly once.
  std::cout << "parent-startup-line\n";
  cvknx::flush_before_fork();

  const pid_t child = ::fork();
  ASSERT_GE(child, 0) << "fork failed";
  if (child == 0) {
    // Child: write one line, then exit without running any gtest teardown.
    std::cout << "child-line\n";
    std::cout.flush();
    ::_Exit(0);
  }

  int status = 0;
  ASSERT_EQ(child, ::waitpid(child, &status, 0)) << "waitpid failed";
  ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally";

  std::cout << "parent-tail-line\n";
  std::cout.flush();

  const std::string out = capture.stop_and_read();
  EXPECT_EQ(1U, count_occurrences(out, "parent-startup-line")) << "captured output:\n" << out;
  EXPECT_EQ(1U, count_occurrences(out, "child-line")) << "captured output:\n" << out;
  EXPECT_EQ(1U, count_occurrences(out, "parent-tail-line")) << "captured output:\n" << out;
}

}  // namespace
