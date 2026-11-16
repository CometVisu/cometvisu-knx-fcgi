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
 * @file bus_config.h
 * @brief Transport selection and per-transport settings from the environment.
 *
 * `KNX_TRANSPORT` has **no default** (2.0.0): all three transports are
 * equally supported and the operator must pick one.  An unset or unknown
 * value is a startup error — silently falling back to knxd would turn an
 * `ip-*` misconfiguration into what looks like a bus outage.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cvknx {

/// The transport used to reach the KNX bus.
enum class BusTransport : std::uint8_t {
  Knxd = 0,      ///< eibd binary protocol over a Unix socket (needs knxd).
  IpTunnel = 1,  ///< KNXnet/IP tunnelling client of an interface/router.
  IpRouting = 2  ///< KNXnet/IP multicast routing member.
};

/// Parse a transport name (`knxd`, `ip-tunnel`, `ip-routing`).
[[nodiscard]] std::optional<BusTransport> bus_transport_from_string(std::string_view name);

/// Canonical name of a transport (for logs and `--help`).
[[nodiscard]] const char* bus_transport_to_string(BusTransport transport);

/// Effective bus configuration.
struct BusConfig {
  BusTransport transport = BusTransport::Knxd;

  /// knxd transport: Unix socket path.
  std::string knxd_socket = "/run/knx";

  /// KNXnet/IP: interface/router address (`ip-tunnel`, required).
  std::string ip_host;
  /// KNXnet/IP: UDP port (both IP transports).
  std::uint16_t ip_port = 3671;
  /// `ip-tunnel`: local address advertised in the HPAI (empty = let the OS pick).
  std::string ip_local_addr;
  /// `ip-routing`: multicast group.
  std::string ip_multicast = "224.0.23.12";
  /// `ip-routing`: individual address used as the cEMI source.  A router
  /// rejects a routing indication from 0.0.0, so a real address is required.
  /// Default 15.15.250 (0xFFFA) — the last line of the address space, which is
  /// practically never assigned by ETS.
  std::uint16_t ip_individual_addr = 0xFFFA;
  /// `ip-routing`: interface for IP_MULTICAST_IF and the group join (empty = any).
  std::string ip_iface;
  /// `ip-tunnel`: NAT mode (accept NAT'd HPAIs, keep-alive).
  bool ip_nat = false;
  /// `ip-tunnel`: CONNECTIONSTATE_REQUEST interval in seconds.
  int heartbeat_sec = 60;
  /// `ip-tunnel`: wait for TUNNELLING_ACK before retransmitting.
  int ack_timeout_ms = 1000;
  /// `ip-tunnel`: retransmissions per telegram.
  int retries = 3;

  /// JSON key of the outage block: `"knxd"` for the knxd transport, `"bus"`
  /// for the KNXnet/IP transports (keeps existing CometVisu deployments
  /// unchanged while the direct transports get a neutral name).
  [[nodiscard]] std::string_view outage_key() const;

  /// One-line summary for log lines.
  [[nodiscard]] std::string describe() const;

  /// Environment accessor: returns "" for an unset variable.
  using EnvGetter = std::function<std::string(const char* name)>;

  /// Parse and validate a configuration.
  /// @param get Environment accessor.
  /// @param error Receives a human readable reason on failure.
  /// @return the configuration, or std::nullopt on any error.  Never falls
  ///         back to a default transport.
  [[nodiscard]] static std::optional<BusConfig> parse(const EnvGetter& get, std::string& error);

  /// Convenience wrapper around getenv().
  [[nodiscard]] static std::optional<BusConfig> from_env(std::string& error);
};

}  // namespace cvknx
