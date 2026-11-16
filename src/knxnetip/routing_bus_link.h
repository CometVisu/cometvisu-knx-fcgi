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
 * @file routing_bus_link.h
 * @brief KNXnet/IP IP routing transport (multicast, fire-and-forget).
 *
 * One UDP socket, owned by the cache reader, joins the routing multicast
 * group and sends ROUTING_INDICATION datagrams (TTL 16, loop enabled).  There
 * is no connection state, no sequence number and no ACK: routing is
 * stateless.
 *
 * Own echoes: the loop is enabled so a local router/knxd can see our frames.
 * Every participant on a routing backbone acts as a router, so the cEMI of a
 * ROUTING_INDICATION must be an L_Data.ind (0x29) — an L_Data.req (0x11) is
 * silently dropped by knxd and by real interfaces.  Our own frame comes back
 * as L_Data.ind and is treated as the bus echo of the write: it is pushed into
 * the shared cache exactly once, which is what announces the new value to the
 * other clients.
 */

#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bus/bus_config.h"
#include "bus/bus_link.h"

namespace cvknx {

/// Routing socket parameters.
struct RoutingOptions {
  /// Destination address (multicast group in production, unicast in tests).
  std::string address = "224.0.23.12";
  /// Destination UDP port (and local port unless bind_port is set).
  std::uint16_t port = 3671;
  /// Interface name for the group join / IP_MULTICAST_IF (empty = default).
  std::string iface;
  /// Individual address used as the cEMI source of our routing indications.
  /// A router rejects 0.0.0 and its own tunnel client addresses.
  std::uint16_t source_address = 0xFFFA;  // 15.15.250
  /// Local bind port; 0 = let the kernel pick an ephemeral port (loopback
  /// tests).  RoutingOptions::from_config() binds the configured port.
  std::uint16_t bind_port = 0;

  [[nodiscard]] static RoutingOptions from_config(const BusConfig& config);
};
class RoutingBusLink : public BusLink {
public:
  explicit RoutingBusLink(const RoutingOptions& options);
  ~RoutingBusLink() override;

  RoutingBusLink(const RoutingBusLink&) = delete;
  RoutingBusLink& operator=(const RoutingBusLink&) = delete;

  [[nodiscard]] bool connect() override;
  void disconnect() override;
  [[nodiscard]] bool is_connected() const override { return fd_ >= 0; }

  [[nodiscard]] bool send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) override;
  [[nodiscard]] bool poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) override;

  [[nodiscard]] int get_fd() const override { return fd_; }
  [[nodiscard]] std::uint64_t get_telegram_count() const override { return telegram_count_; }
  void set_nonblocking(bool enable) override;
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;
  [[nodiscard]] std::string label() const override;
  /// Routing is stateless: there is no handshake and no outage detection.
  [[nodiscard]] bool is_stateless() const override { return true; }

  /// Port the socket is actually bound to (useful for tests and logs).
  [[nodiscard]] std::uint16_t local_port() const { return local_port_; }

  /// Destination address of a received datagram (host byte order), taken from
  /// its `IP_PKTINFO` control message.
  ///
  /// A routing participant cannot tell sender addresses apart (every router
  /// on the backbone is a legitimate source), but it can verify that the
  /// datagram was actually sent to the KNX routing destination — a unicast
  /// packet sprayed at our port by an unrelated host is dropped instead of
  /// being parsed as bus traffic.
  ///
  /// @return the destination address, or std::nullopt when the kernel supplied
  ///         none (such a datagram cannot be verified and is dropped).
  [[nodiscard]] static std::optional<std::uint32_t> destination_of(struct msghdr& message);

private:
  RoutingOptions options_;
  int fd_ = -1;
  std::uint16_t local_port_ = 0;
  std::uint64_t telegram_count_ = 0;
  /// Parsed destination address (host byte order), valid while connected.
  std::uint32_t destination_ = 0;
};

}  // namespace cvknx
