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
 * @file knxnetip_blackbox.h
 * @brief Harness for black-box tests of the direct KNXnet/IP transports.
 *
 * The unit tests drive the transport classes in-process, so they can only
 * prove that the code is self-consistent.  These helpers test the *deployed*
 * behaviour instead:
 *
 *   - a real `knxd` provides a KNXnet/IP router (`-b ip:<group>:<port>`) and/or
 *     a KNXnet/IP tunnel server (`-T -S <group>:<port>`);
 *   - the real `cometvisu-knx-fcgi` binary is started as a separate process and
 *     spoken to over a direct FastCGI socket;
 *   - telegrams are injected on knxd's Unix socket (`knxtool groupswrite`), and
 *     a member of the routing group monitors what the backend puts on the wire;
 *   - `knxtool groupcacheread` is the independent witness that a write really
 *     reached the KNX bus.
 *
 * Everything is isolated per test: unique Unix socket, unique multicast group,
 * unique UDP ports.  Tests skip (never fail) when `knxd`/`knxtool` are missing.
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cvknx::blackbox {

using Bytes = std::vector<std::uint8_t>;
using Env = std::vector<std::pair<std::string, std::string>>;

/// True when both `knxd` and `knxtool` can be executed.
[[nodiscard]] bool knxd_available();

/// Path of an executable in `PATH`, or "" when it is not installed.
[[nodiscard]] std::string find_executable(const std::string& name);

/// A free UDP port (bound with port 0, then released).
[[nodiscard]] std::uint16_t free_udp_port();

/// A free multicast group for this process (unique per call).
[[nodiscard]] std::string unique_multicast_group();

/// A unique Unix socket path.
[[nodiscard]] std::string unique_socket_path(const std::string& tag);

/// Group address "1/2/3" -> 16-bit value.
[[nodiscard]] std::uint16_t group_address_value(const std::string& text);

/// Individual address "1.1.250" -> 16-bit value.
[[nodiscard]] std::uint16_t individual_address_value(const std::string& text);

// ===========================================================================
// knxd
// ===========================================================================

/// How knxd is wired to the KNX bus.
enum class KnxdMode : std::uint8_t {
  /// KNXnet/IP router: `-b ip:<group>:<port>:<iface>` (no tunnel server).
  Router = 0,
  /// KNXnet/IP tunnel server on its own port plus an `ip:` router backend.
  RouterAndTunnel = 1,
  /// Tunnel server only (`-T -S ...`) — the classic KNXnet/IP interface.
  TunnelOnly = 2
};

struct KnxdOptions {
  KnxdMode mode = KnxdMode::Router;
  std::string group = "239.255.99.1";
  std::uint16_t port = 3671;
  std::string iface = "lo";
  /// Only used by RouterAndTunnel / TunnelOnly.
  std::uint16_t tunnel_port = 0;
};

/**
 * A supervised knxd process.
 *
 * knxd needs `-S` to be the last option group ("You need -S after -D/-T/-R"),
 * which this class takes care of.
 */
class Knxd {
public:
  explicit Knxd(KnxdOptions options);
  ~Knxd();

  Knxd(const Knxd&) = delete;
  Knxd& operator=(const Knxd&) = delete;

  /// Start knxd and wait for its Unix socket (and tunnel port).
  [[nodiscard]] bool start();

  /// SIGTERM + wait; safe to call repeatedly.
  void stop();

  /// Stop and start again on the same socket/port (bus outage simulation).
  [[nodiscard]] bool restart();

  [[nodiscard]] bool running() const;
  [[nodiscard]] const std::string& socket_path() const { return socket_path_; }
  [[nodiscard]] std::uint16_t tunnel_port() const { return options_.tunnel_port; }
  [[nodiscard]] std::string log() const;

  /// `knxtool groupswrite local:<socket> <address> <value>`.
  [[nodiscard]] bool groupswrite(const std::string& group_address, int value);

  /// Hex value the address has in knxd's group cache ("" when there is none).
  [[nodiscard]] std::string groupcache_read(const std::string& group_address);

private:
  [[nodiscard]] bool wait_ready(int timeout_ms);
  [[nodiscard]] std::vector<std::string> command_line() const;
  void collect_output();

  KnxdOptions options_;
  std::string socket_path_;
  std::string log_path_;
  pid_t pid_ = -1;
};

// ===========================================================================
// Backend process
// ===========================================================================

struct BackendOptions {
  /// Extra environment for the child (on top of the defaults below).
  Env env;
  int workers = 1;
  int longpoll_seconds = 30;
  bool debug = false;
};

/// The real `cometvisu-knx-fcgi` binary, started with a direct FCGI socket.
class Backend {
public:
  explicit Backend(BackendOptions options);
  ~Backend();

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  /// Start and wait until the FCGI socket accepts connections.
  [[nodiscard]] bool start();
  void stop();
  [[nodiscard]] bool running() const;

  [[nodiscard]] const std::string& socket_path() const { return socket_path_; }
  [[nodiscard]] std::string log() const;
  /// Wait until the accumulated stdout/stderr contains @p needle.
  [[nodiscard]] bool wait_for_log(const std::string& needle, int timeout_ms = 5000);
  [[nodiscard]] pid_t pid() const { return pid_; }

private:
  [[nodiscard]] std::vector<std::string> command_line() const;

  BackendOptions options_;
  std::string socket_path_;
  pid_t pid_ = -1;
};

// ===========================================================================
// FastCGI client
// ===========================================================================

struct FcgiResponse {
  int status = 0;
  std::string body;
};

/// Send one request over a direct FastCGI socket (fresh connection).
/// @param target path plus query string, e.g. "/r?a=1/2/3&t=-1".
[[nodiscard]] FcgiResponse fcgi_request(const std::string& socket_path, const std::string& target,
                                        const std::string& method = "GET",
                                        const std::string& body = "");

/// Position ("i") from a /r response, 0 when absent.
[[nodiscard]] std::uint32_t response_position(const FcgiResponse& response);

/// Append the protocol-mandated session parameter (`s=0`, the anonymous
/// session) unless @p query already carries an `s=` — a test that asserts the
/// behaviour of an invalid session keeps its own value.
[[nodiscard]] std::string with_session(const std::string& query);
/// The "d" object as raw text ("" when absent).
[[nodiscard]] std::string response_data_object(const FcgiResponse& response);

// ===========================================================================
// KNXnet/IP routing monitor / injector
// ===========================================================================

struct RoutingFrame {
  std::uint16_t service = 0;
  Bytes datagram;
  Bytes cemi;
  /// Source of the datagram (the process that put it on the wire).
  std::string source_ip;
  std::uint16_t source_port = 0;
  /// IPv4 TTL of the datagram (-1 when unknown).
  int ttl = -1;
};

/**
 * A member of a KNXnet/IP routing group.
 *
 * Receives every ROUTING_INDICATION on the group (including the local ones,
 * because producers run with IP_MULTICAST_LOOP enabled) and can inject
 * telegrams from a *different* source port — exactly what a foreign KNXnet/IP
 * router would do.
 */
class RoutingMonitor {
public:
  RoutingMonitor(std::string group, std::uint16_t port, std::string iface_address);
  ~RoutingMonitor();

  RoutingMonitor(const RoutingMonitor&) = delete;
  RoutingMonitor& operator=(const RoutingMonitor&) = delete;

  [[nodiscard]] bool open();

  /// Send a ROUTING_INDICATION carrying @p cemi from an ephemeral port.
  [[nodiscard]] bool inject(const Bytes& cemi);

  /// Convenience: inject an L_Data.ind group write.
  [[nodiscard]] bool inject_write(std::uint16_t group, const Bytes& apdu,
                                  std::uint16_t source_ia = 0x1102);

  /// Frames for @p group (0 = any) collected until @p timeout_ms elapsed or
  /// @p expected frames arrived (-1 = until the timeout).
  [[nodiscard]] std::vector<RoutingFrame> collect(int timeout_ms, std::uint16_t group = 0,
                                                  int expected = -1);

  /// Drop everything currently queued.
  void drain();

private:
  [[nodiscard]] bool receive_one(RoutingFrame& out, int timeout_ms);

  std::string group_;
  std::uint16_t port_;
  std::string iface_address_;
  int rx_fd_ = -1;
  int tx_fd_ = -1;
};

// ===========================================================================
// Small helpers
// ===========================================================================

/// Build a cEMI L_Data.ind for a group value write (what a router publishes).
[[nodiscard]] Bytes cemi_group_indication(std::uint16_t source_ia, std::uint16_t group,
                                          const Bytes& apdu);

/// Build a cEMI L_Data.req (what a tunneling client sends).
[[nodiscard]] Bytes cemi_group_request(std::uint16_t source_ia, std::uint16_t group,
                                       const Bytes& apdu);

/// Wrap a cEMI frame in a ROUTING_INDICATION with an explicit service type.
[[nodiscard]] Bytes routing_datagram(const Bytes& cemi, std::uint16_t service = 0x0530);

}  // namespace cvknx::blackbox
