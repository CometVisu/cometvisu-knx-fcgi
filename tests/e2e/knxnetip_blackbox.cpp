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
 * @file knxnetip_blackbox.cpp
 * @brief Harness for black-box tests of the direct KNXnet/IP transports.
 */

#include "knxnetip_blackbox.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace cvknx::blackbox {
namespace {

using Clock = std::chrono::steady_clock;

/// Test-local unique id source (keeps sockets and groups apart).
std::atomic<int> g_unique{0};

int next_unique() {
  return g_unique.fetch_add(1);
}

std::string read_file_or_empty(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/// Wait until @p predicate is true or the deadline expires.
template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (Clock::now() < deadline);
  return predicate();
}

/// Close a file descriptor, ignoring an already closed fd.
void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

/// Stop a child process (SIGTERM, then SIGKILL) and reap it.
void terminate_child(pid_t& pid) {
  if (pid <= 0) {
    return;
  }
  ::kill(pid, SIGTERM);
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  int status = 0;
  while (Clock::now() < deadline) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      pid = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(pid, SIGKILL);
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  pid = -1;
}

/// Fork/exec @p argv with @p env, output redirected to @p log_path.
pid_t spawn_child(const std::vector<std::string>& argv, const Env& env,
                  const std::string& log_path) {
  if (argv.empty()) {
    return -1;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }
    const int log = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log >= 0) {
      ::dup2(log, STDOUT_FILENO);
      ::dup2(log, STDERR_FILENO);
      ::close(log);
    }
    // A test child must not inherit the parent's transport settings.
    ::unsetenv("KNX_TRANSPORT");
    ::unsetenv("KNX_IP_HOST");
    ::unsetenv("KNX_IP_PORT");
    ::unsetenv("KNX_IP_MULTICAST");
    ::unsetenv("KNX_IP_IFACE");
    ::unsetenv("KNX_IP_INDIVIDUAL_ADDR");
    ::unsetenv("KNXD_SOCKET");
    ::unsetenv("FCGI_SOCKET");
    ::unsetenv("DEBUG_BACKEND");
    ::unsetenv("KNX_COVERAGE");
    for (const auto& [name, value] : env) {
      ::setenv(name.c_str(), value.c_str(), 1);
    }
    std::vector<char*> pointers;
    pointers.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      pointers.push_back(const_cast<char*>(arg.c_str()));
    }
    pointers.push_back(nullptr);
    ::execv(pointers[0], pointers.data());
    ::_exit(127);
  }
  return pid;
}

/// Run a command to completion, returning its stdout (stderr suppressed).
std::string run_command(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return {};
  }
  int out_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    return {};
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return {};
  }
  if (pid == 0) {
    ::dup2(out_pipe[1], STDOUT_FILENO);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    std::vector<char*> pointers;
    pointers.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      pointers.push_back(const_cast<char*>(arg.c_str()));
    }
    pointers.push_back(nullptr);
    ::execv(pointers[0], pointers.data());
    ::_exit(127);
  }
  ::close(out_pipe[1]);
  std::string output;
  char buffer[512] = {};
  ssize_t n = 0;
  while ((n = ::read(out_pipe[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, static_cast<size_t>(n));
  }
  ::close(out_pipe[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return output;
}

/// Connect to a Unix FastCGI socket; returns -1 when it is not listening yet.
int connect_unix(const std::string& path, int receive_timeout_sec) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  ::timeval timeout{receive_timeout_sec, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  return fd;
}

std::string fcgi_record(std::uint8_t type, const std::string& content) {
  std::string out{'\x01',
                  static_cast<char>(type),
                  '\x00',
                  '\x01',
                  static_cast<char>((content.size() >> 8) & 0xFF),
                  static_cast<char>(content.size() & 0xFF),
                  '\x00',
                  '\x00'};
  return out + content;
}

std::string fcgi_param(const std::string& name, const std::string& value) {
  return std::string{static_cast<char>(name.size()), static_cast<char>(value.size())} + name +
         value;
}

}  // namespace

// ===========================================================================
// Small helpers
// ===========================================================================

bool knxd_available() {
  return !find_executable("knxd").empty() && !find_executable("knxtool").empty();
}

std::string find_executable(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return {};
  }
  std::string directories{path};
  std::size_t start = 0;
  while (start <= directories.size()) {
    const std::size_t end = directories.find(':', start);
    const std::string directory =
        directories.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!directory.empty()) {
      const std::string candidate = directory + "/" + name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

std::uint16_t free_udp_port() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 0;
  }
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  (void)::getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), &length);
  ::close(fd);
  return ntohs(address.sin_port);
}

std::string unique_multicast_group() {
  const int id = next_unique();
  // 239.255.<pid-derived>.<id>: unique per process and per test, and always a
  // local-scope group that never leaves the container.
  const int third = 30 + (static_cast<int>(::getpid()) % 180);
  const int fourth = 1 + (id % 254);
  return "239.255." + std::to_string(third) + "." + std::to_string(fourth);
}

std::string unique_socket_path(const std::string& tag) {
  return "/tmp/cvknx-bb-" + tag + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(next_unique()) + ".sock";
}

std::uint16_t group_address_value(const std::string& text) {
  unsigned x = 0;
  unsigned y = 0;
  unsigned z = 0;
  if (std::sscanf(text.c_str(), "%u/%u/%u", &x, &y, &z) != 3) {
    return 0;
  }
  return static_cast<std::uint16_t>(((x & 0x1F) << 11) | ((y & 0x07) << 8) | (z & 0xFF));
}

std::uint16_t individual_address_value(const std::string& text) {
  unsigned area = 0;
  unsigned line = 0;
  unsigned device = 0;
  if (std::sscanf(text.c_str(), "%u.%u.%u", &area, &line, &device) != 3) {
    return 0;
  }
  return static_cast<std::uint16_t>(((area & 0x0F) << 12) | ((line & 0x0F) << 8) | (device & 0xFF));
}

// ===========================================================================
// knxd
// ===========================================================================

Knxd::Knxd(KnxdOptions options) : options_(std::move(options)) {
  socket_path_ = unique_socket_path("knxd");
  log_path_ = socket_path_ + ".log";
}

Knxd::~Knxd() {
  stop();
  (void)::unlink(socket_path_.c_str());
  (void)::unlink(log_path_.c_str());
}

std::vector<std::string> Knxd::command_line() const {
  const std::string knxd = find_executable("knxd");
  std::vector<std::string> argv{knxd, "-e",         "1.1.220", "-E", "1.1.221:10",
                                "-u", socket_path_, "-c100",   "-f", "9"};
  if (options_.mode == KnxdMode::TunnelOnly) {
    argv.insert(argv.end(), {"-T", "-D"});
  } else {
    // The `ip:` layer-2 driver is the KNXnet/IP *router*: it joins the group
    // and forwards between the multicast backbone and knxd's bus.
    argv.insert(argv.end(), {"-b", "ip:" + options_.group + ":" + std::to_string(options_.port) +
                                       ":" + options_.iface});
    if (options_.mode == KnxdMode::RouterAndTunnel) {
      argv.insert(argv.end(), {"-T", "-D"});
    }
  }
  if (options_.mode != KnxdMode::Router) {
    // `-S` must be the last option ("You need -S after -D/-T/-R").
    argv.insert(argv.end(), {"-S", options_.group + ":" + std::to_string(options_.tunnel_port)});
  }
  return argv;
}

bool Knxd::start() {
  stop();
  (void)::unlink(socket_path_.c_str());
  const Env env;  // no extra environment
  pid_ = spawn_child(command_line(), env, log_path_);
  if (pid_ <= 0) {
    return false;
  }
  return wait_ready(8000);
}

bool Knxd::wait_ready(int timeout_ms) {
  const bool ready = wait_until(
      [this] {
        if (!running()) {
          return false;
        }
        return ::access(socket_path_.c_str(), F_OK) == 0;
      },
      timeout_ms);
  if (!ready) {
    return false;
  }
  // The Unix socket exists before the KNXnet/IP server sockets are bound; a
  // short settle avoids racing the first CONNECT_REQUEST.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return running();
}

void Knxd::stop() {
  terminate_child(pid_);
}

bool Knxd::restart() {
  stop();
  return start();
}

bool Knxd::running() const {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
  return waited == 0;
}

std::string Knxd::log() const {
  return read_file_or_empty(log_path_);
}

bool Knxd::groupswrite(const std::string& group_address, int value) {
  const std::string knxtool = find_executable("knxtool");
  if (knxtool.empty()) {
    return false;
  }
  run_command(
      {knxtool, "groupswrite", "local:" + socket_path_, group_address, std::to_string(value)});
  // knxd needs a moment to put the telegram on the bus.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  return true;
}

std::string Knxd::groupcache_read(const std::string& group_address) {
  const std::string knxtool = find_executable("knxtool");
  if (knxtool.empty()) {
    return {};
  }
  const std::string output =
      run_command({knxtool, "groupcacheread", "local:" + socket_path_, group_address});
  if (output.find("failed") != std::string::npos) {
    return {};  // no cache entry for this address
  }
  // "Write from 1.1.221: 01" / "Read from ...: 0C 6F" — the value follows the
  // last colon, may be grouped in bytes and is upper case.  Normalize it to
  // the lower-case hex CometVisu uses.
  const std::size_t colon = output.rfind(':');
  if (colon == std::string::npos) {
    return {};
  }
  std::string value;
  for (const char c : output.substr(colon + 1)) {
    if (std::isxdigit(static_cast<unsigned char>(c)) != 0) {
      value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  if (value.size() < 2 || (value.size() % 2) != 0) {
    return {};
  }
  return value;
}

// ===========================================================================
// Backend
// ===========================================================================

Backend::Backend(BackendOptions options) : options_(std::move(options)) {
  socket_path_ = unique_socket_path("fcgi");
}

Backend::~Backend() {
  stop();
}

std::vector<std::string> Backend::command_line() const {
  return {CVKNX_BINARY};
}

bool Backend::start() {
  stop();
  (void)::unlink(socket_path_.c_str());

  Env env;
  env.emplace_back("FCGI_SOCKET", socket_path_);
  env.emplace_back("FCGI_WORKERS", std::to_string(options_.workers));
  env.emplace_back("LONGPOLL_TIMEOUT_SEC", std::to_string(options_.longpoll_seconds));
  if (options_.debug) {
    env.emplace_back("DEBUG_BACKEND", "1");
  }
  for (const auto& entry : options_.env) {
    env.push_back(entry);
  }
  pid_ = spawn_child(command_line(), env, socket_path_ + ".log");
  if (pid_ <= 0) {
    return false;
  }
  return wait_until(
      [this] {
        if (!running()) {
          return false;
        }
        const int fd = connect_unix(socket_path_, 1);
        if (fd < 0) {
          return false;
        }
        ::close(fd);
        return true;
      },
      10000);
}

void Backend::stop() {
  terminate_child(pid_);
}

bool Backend::running() const {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  return ::waitpid(pid_, &status, WNOHANG) == 0;
}

std::string Backend::log() const {
  return read_file_or_empty(socket_path_ + ".log");
}

bool Backend::wait_for_log(const std::string& needle, int timeout_ms) {
  return wait_until([this, &needle] { return log().find(needle) != std::string::npos; },
                    timeout_ms);
}

// ===========================================================================
// FastCGI client
// ===========================================================================

FcgiResponse fcgi_request(const std::string& socket_path, const std::string& target,
                          const std::string& method, const std::string& body) {
  FcgiResponse response;
  const int fd = connect_unix(socket_path, 15);
  if (fd < 0) {
    return response;
  }

  const std::size_t question = target.find('?');
  const std::string path = target.substr(0, question);
  const std::string query =
      question == std::string::npos ? std::string{} : target.substr(question + 1);

  std::string request = fcgi_record(0x01, std::string("\x00\x01\x00\x00\x00\x00\x00\x00", 8));
  std::string params = fcgi_param("REQUEST_METHOD", method) + fcgi_param("REQUEST_URI", target) +
                       fcgi_param("SCRIPT_NAME", path) + fcgi_param("QUERY_STRING", query) +
                       fcgi_param("GATEWAY_INTERFACE", "CGI/1.1") +
                       fcgi_param("CONTENT_TYPE", "application/x-www-form-urlencoded") +
                       fcgi_param("CONTENT_LENGTH", std::to_string(body.size()));
  request += fcgi_record(0x04, params);
  request += fcgi_record(0x04, "");
  request += fcgi_record(0x05, body);
  request += fcgi_record(0x05, "");

  std::size_t sent = 0;
  while (sent < request.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t written = ::write(fd, request.data() + sent, request.size() - sent);
    if (written <= 0) {
      ::close(fd);
      return response;
    }
    sent += static_cast<size_t>(written);
  }

  std::string raw;
  char buffer[1024] = {};
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    raw.append(buffer, static_cast<size_t>(n));
  }
  ::close(fd);

  const std::size_t status_at = raw.find("Status: ");
  if (status_at != std::string::npos) {
    response.status = std::atoi(raw.c_str() + status_at + 8);
  }
  const std::size_t headers_end = raw.find("\r\n\r\n");
  response.body = headers_end == std::string::npos ? std::string{} : raw.substr(headers_end + 4);
  return response;
}

std::string with_session(const std::string& query) {
  // The protocol requires `s` on every /r and /w.  A test that wants an
  // invalid session supplies its own value and keeps it.
  if (query.find("s=") != std::string::npos) {
    return query;
  }
  return query.empty() ? "s=0" : query + "&s=0";
}

std::uint32_t response_position(const FcgiResponse& response) {
  const std::size_t pos = response.body.find("\"i\":");
  if (pos == std::string::npos) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::strtoul(response.body.c_str() + pos + 4, nullptr, 10));
}

std::string response_data_object(const FcgiResponse& response) {
  const std::size_t start = response.body.find("\"d\":{");
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end = response.body.find('}', start);
  if (end == std::string::npos) {
    return {};
  }
  return response.body.substr(start + 4, end - start - 3);
}

// ===========================================================================
// Routing monitor / injector
// ===========================================================================

RoutingMonitor::RoutingMonitor(std::string group, std::uint16_t port, std::string iface_address)
    : group_(std::move(group)), port_(port), iface_address_(std::move(iface_address)) {}

RoutingMonitor::~RoutingMonitor() {
  close_fd(rx_fd_);
  close_fd(tx_fd_);
}

bool RoutingMonitor::open() {
  close_fd(rx_fd_);
  close_fd(tx_fd_);

  rx_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (rx_fd_ < 0) {
    return false;
  }
  int enable = 1;
  (void)::setsockopt(rx_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  (void)::setsockopt(rx_fd_, IPPROTO_IP, IP_RECVTTL, &enable, sizeof(enable));
  // Make sure a foreign frame from another local interface is visible too.
  (void)::setsockopt(rx_fd_, IPPROTO_IP, IP_MULTICAST_ALL, &enable, sizeof(enable));

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port_);
  if (::bind(rx_fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
    close_fd(rx_fd_);
    return false;
  }

  struct ip_mreqn membership {};
  membership.imr_multiaddr.s_addr = ::inet_addr(group_.c_str());
  membership.imr_address.s_addr = ::inet_addr(iface_address_.c_str());
  if (::setsockopt(rx_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
    close_fd(rx_fd_);
    return false;
  }

  tx_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (tx_fd_ < 0) {
    close_fd(rx_fd_);
    return false;
  }
  struct sockaddr_in source {};
  source.sin_family = AF_INET;
  source.sin_addr.s_addr = ::inet_addr(iface_address_.c_str());
  source.sin_port = 0;
  if (::bind(tx_fd_, reinterpret_cast<struct sockaddr*>(&source), sizeof(source)) != 0) {
    close_fd(rx_fd_);
    close_fd(tx_fd_);
    return false;
  }
  struct ip_mreqn outgoing {};
  outgoing.imr_address.s_addr = ::inet_addr(iface_address_.c_str());
  (void)::setsockopt(tx_fd_, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, sizeof(outgoing));
  unsigned char ttl = 16;
  (void)::setsockopt(tx_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  return true;
}

bool RoutingMonitor::inject(const Bytes& cemi) {
  if (tx_fd_ < 0) {
    return false;
  }
  const Bytes datagram = routing_datagram(cemi);
  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = ::inet_addr(group_.c_str());
  destination.sin_port = htons(port_);
  const ssize_t sent =
      ::sendto(tx_fd_, datagram.data(), datagram.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
  return sent == static_cast<ssize_t>(datagram.size());
}

bool RoutingMonitor::inject_write(std::uint16_t group, const Bytes& apdu, std::uint16_t source_ia) {
  return inject(cemi_group_indication(source_ia, group, apdu));
}

void RoutingMonitor::drain() {
  (void)collect(0);
}

bool RoutingMonitor::receive_one(RoutingFrame& out, int timeout_ms) {
  if (rx_fd_ < 0) {
    return false;
  }
  struct pollfd entry {};
  entry.fd = rx_fd_;
  entry.events = POLLIN;
  const int ready = ::poll(&entry, 1, timeout_ms);
  if (ready <= 0) {
    return false;
  }

  std::uint8_t buffer[2048] = {};  // NOLINT(modernize-avoid-c-arrays)
  std::uint8_t control[256] = {};  // NOLINT(modernize-avoid-c-arrays)
  struct sockaddr_in from {};
  struct iovec vector {};
  vector.iov_base = buffer;
  vector.iov_len = sizeof(buffer);
  struct msghdr message {};
  message.msg_name = &from;
  message.msg_namelen = sizeof(from);
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);

  const ssize_t received = ::recvmsg(rx_fd_, &message, 0);
  if (received <= 0) {
    return false;
  }

  out = RoutingFrame{};
  out.datagram.assign(buffer, buffer + received);
  out.source_ip = ::inet_ntoa(from.sin_addr);
  out.source_port = ntohs(from.sin_port);
  for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_TTL) {
      int ttl = 0;
      std::memcpy(&ttl, CMSG_DATA(header), sizeof(ttl));
      out.ttl = ttl;
    }
  }

  if (out.datagram.size() < 6) {
    return true;  // not a KNXnet/IP frame; let the caller filter
  }
  out.service = static_cast<std::uint16_t>((out.datagram[2] << 8) | out.datagram[3]);
  // The body is a bare cEMI frame for every ROUTING_* service; keep it even
  // when the service code is wrong, so a test can report what was on the wire.
  out.cemi.assign(out.datagram.begin() + 6, out.datagram.end());
  return true;
}

std::vector<RoutingFrame> RoutingMonitor::collect(int timeout_ms, std::uint16_t group,
                                                  int expected) {
  std::vector<RoutingFrame> frames;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    const auto now = Clock::now();
    const int remaining =
        now >= deadline
            ? 0
            : static_cast<int>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    RoutingFrame frame;
    if (!receive_one(frame, remaining)) {
      if (Clock::now() >= deadline) {
        break;
      }
      continue;
    }
    if (group != 0) {
      // cEMI L_Data: destination is at offset 6..7.
      if (frame.cemi.size() < 8) {
        continue;
      }
      const auto destination = static_cast<std::uint16_t>((frame.cemi[6] << 8) | frame.cemi[7]);
      if (destination != group) {
        continue;
      }
    }
    frames.push_back(frame);
    if (expected > 0 && static_cast<int>(frames.size()) >= expected) {
      break;
    }
  }
  return frames;
}

// ===========================================================================
// Frame builders
// ===========================================================================

Bytes cemi_group_indication(std::uint16_t source_ia, std::uint16_t group, const Bytes& apdu) {
  Bytes frame{0x29, 0x00, 0xBC, 0xE0};
  frame.push_back(static_cast<std::uint8_t>((source_ia >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(source_ia & 0xFF));
  frame.push_back(static_cast<std::uint8_t>((group >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(group & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(apdu.size() - 1));
  frame.insert(frame.end(), apdu.begin(), apdu.end());
  return frame;
}

Bytes cemi_group_request(std::uint16_t source_ia, std::uint16_t group, const Bytes& apdu) {
  Bytes frame = cemi_group_indication(source_ia, group, apdu);
  frame[0] = 0x11;
  return frame;
}

Bytes routing_datagram(const Bytes& cemi, std::uint16_t service) {
  const auto total = static_cast<std::uint16_t>(6 + cemi.size());
  Bytes frame{0x06,
              0x10,
              static_cast<std::uint8_t>(service >> 8),
              static_cast<std::uint8_t>(service & 0xFF),
              static_cast<std::uint8_t>(total >> 8),
              static_cast<std::uint8_t>(total & 0xFF)};
  frame.insert(frame.end(), cemi.begin(), cemi.end());
  return frame;
}

}  // namespace cvknx::blackbox
