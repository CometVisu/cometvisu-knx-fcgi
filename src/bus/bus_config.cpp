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
 * @file bus_config.cpp
 * @brief Environment parsing for the transport configuration.
 */

#include "bus/bus_config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cvknx {
namespace {

/// Parse a decimal integer within [min, max].  Rejects trailing garbage and
/// empty values — a typo must never be silently replaced by a default.
bool parse_int(const std::string& text, int min_val, int max_val, int& out) {
  if (text.empty()) {  // GCOV_EXCL_BR_LINE - every caller checks emptiness first
    return false;      // GCOV_EXCL_LINE
  }
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
    return false;
  }
  if (value < static_cast<long>(min_val) || value > static_cast<long>(max_val)) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/// Parse a boolean flag ("1"/"true"/"yes"/"on" and the opposites).
bool parse_bool(const std::string& text, bool& out) {
  if (text == "1" || text == "true" || text == "yes" || text == "on") {
    out = true;
    return true;
  }
  if (text == "0" || text == "false" || text == "no" || text == "off") {
    out = false;
    return true;
  }
  return false;
}

std::string error_int(const char* name, const std::string& value, int min_val, int max_val) {
  return std::string{"invalid "} + name + " '" + value + "' (expected " + std::to_string(min_val) +
         ".." + std::to_string(max_val) + ")";
}

/// Parse an individual address `A.L.D` (0..15 . 0..15 . 1..255).
/// The all-zero address is not a device address and is rejected.
bool parse_individual_address(const std::string& text, std::uint16_t& out) {
  unsigned area = 0;
  unsigned line = 0;
  unsigned device = 0;
  char extra = 0;
  // The trailing %c turns "1.1.2.3" or "1.1.2x" into 4 conversions, so a
  // value that merely *starts* like an address is rejected as a whole.
  if (std::sscanf(text.c_str(), "%u.%u.%u%c", &area, &line, &device, &extra) != 3) {
    return false;
  }
  if (area > 15U || line > 15U || device == 0U || device > 255U) {
    return false;
  }
  out = static_cast<std::uint16_t>(((area & 0x0FU) << 12) | ((line & 0x0FU) << 8) | device);
  // device != 0 above already guarantees a non-zero address.
  return true;
}

}  // namespace

std::optional<BusTransport> bus_transport_from_string(std::string_view name) {
  if (name == "knxd") {
    return BusTransport::Knxd;
  }
  if (name == "ip-tunnel") {
    return BusTransport::IpTunnel;
  }
  if (name == "ip-routing") {
    return BusTransport::IpRouting;
  }
  return std::nullopt;
}

const char* bus_transport_to_string(BusTransport transport) {
  switch (transport) {  // GCOV_EXCL_BR_LINE - exhaustive enum switch
    case BusTransport::Knxd:
      return "knxd";
    case BusTransport::IpTunnel:
      return "ip-tunnel";
    case BusTransport::IpRouting:
      return "ip-routing";
  }
  return "unknown";  // GCOV_EXCL_LINE - unreachable: the enum is exhaustive
}

std::string_view BusConfig::outage_key() const {
  return transport == BusTransport::Knxd ? "knxd" : "bus";
}

std::string BusConfig::describe() const {
  std::string text = "transport=";
  text += bus_transport_to_string(transport);
  switch (transport) {  // GCOV_EXCL_BR_LINE - exhaustive enum switch
    case BusTransport::Knxd:
      text += " socket=" + knxd_socket;
      break;
    case BusTransport::IpTunnel:
      text += " host=" + ip_host + ":" + std::to_string(ip_port);
      if (!ip_local_addr.empty()) {
        text += " local=" + ip_local_addr;
      }
      if (ip_nat) {
        text += " nat=1";
      }
      break;
    case BusTransport::IpRouting:
      text += " multicast=" + ip_multicast + ":" + std::to_string(ip_port);
      if (!ip_iface.empty()) {
        text += " iface=" + ip_iface;
      }
      text += " addr=" + std::to_string((ip_individual_addr >> 12) & 0x0F) + "." +
              std::to_string((ip_individual_addr >> 8) & 0x0F) + "." +
              std::to_string(ip_individual_addr & 0xFF);
      break;
  }
  return text;
}  // GCOV_EXCL_LINE - closing brace, no statement

std::optional<BusConfig> BusConfig::parse(const EnvGetter& get, std::string& error) {
  error.clear();
  BusConfig cfg;

  // ---- Transport (mandatory) ----
  const std::string transport_name = get("KNX_TRANSPORT");
  if (transport_name.empty()) {
    error = "KNX_TRANSPORT is required (knxd|ip-tunnel|ip-routing)";
    return std::nullopt;
  }
  const auto transport = bus_transport_from_string(transport_name);
  if (!transport) {
    error = "unknown KNX_TRANSPORT '" + transport_name + "' (valid: knxd, ip-tunnel, ip-routing)";
    return std::nullopt;
  }
  cfg.transport = *transport;

  // ---- knxd ----
  const std::string knxd_socket = get("KNXD_SOCKET");
  if (!knxd_socket.empty()) {
    cfg.knxd_socket = knxd_socket;
  }

  // ---- KNXnet/IP ----
  cfg.ip_host = get("KNX_IP_HOST");
  cfg.ip_local_addr = get("KNX_IP_LOCAL_ADDR");
  const std::string multicast = get("KNX_IP_MULTICAST");
  if (!multicast.empty()) {
    cfg.ip_multicast = multicast;
  }
  cfg.ip_iface = get("KNX_IP_IFACE");

  const std::string individual = get("KNX_IP_INDIVIDUAL_ADDR");
  if (!individual.empty() && !parse_individual_address(individual, cfg.ip_individual_addr)) {
    error = "invalid KNX_IP_INDIVIDUAL_ADDR '" + individual + "' (expected A.L.D, e.g. 15.15.250)";
    return std::nullopt;
  }

  const std::string port = get("KNX_IP_PORT");
  if (!port.empty()) {
    int value = 0;
    if (!parse_int(port, 1, 65535, value)) {
      error = error_int("KNX_IP_PORT", port, 1, 65535);
      return std::nullopt;
    }
    cfg.ip_port = static_cast<std::uint16_t>(value);
  }

  const std::string heartbeat = get("KNX_IP_HEARTBEAT_SEC");
  if (!heartbeat.empty() && !parse_int(heartbeat, 10, 86400, cfg.heartbeat_sec)) {
    error = error_int("KNX_IP_HEARTBEAT_SEC", heartbeat, 10, 86400);
    return std::nullopt;
  }

  const std::string ack_timeout = get("KNX_IP_ACK_TIMEOUT_MS");
  if (!ack_timeout.empty() && !parse_int(ack_timeout, 10, 60000, cfg.ack_timeout_ms)) {
    error = error_int("KNX_IP_ACK_TIMEOUT_MS", ack_timeout, 10, 60000);
    return std::nullopt;
  }

  const std::string retries = get("KNX_IP_RETRIES");
  if (!retries.empty() && !parse_int(retries, 1, 10, cfg.retries)) {
    error = error_int("KNX_IP_RETRIES", retries, 1, 10);
    return std::nullopt;
  }

  const std::string nat = get("KNX_IP_NAT");
  if (!nat.empty() && !parse_bool(nat, cfg.ip_nat)) {
    error = "invalid KNX_IP_NAT '" + nat + "' (expected 0 or 1)";
    return std::nullopt;
  }

  if (cfg.transport == BusTransport::IpTunnel && cfg.ip_host.empty()) {
    error = "KNX_IP_HOST is required for KNX_TRANSPORT=ip-tunnel";
    return std::nullopt;
  }

  return cfg;
}

std::optional<BusConfig> BusConfig::from_env(std::string& error) {
  const EnvGetter get = [](const char* name) -> std::string {
    const char* value = std::getenv(name);
    if (value == nullptr) {
      return {};
    }
    return value;
  };
  return parse(get, error);
}

}  // namespace cvknx
