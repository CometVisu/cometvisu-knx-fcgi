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
 * @file discovery.h
 * @brief KNXnet/IP SEARCH_REQUEST discovery — diagnostics only.
 *
 * Discovery never selects a device automatically: `KNX_TRANSPORT` and
 * `KNX_IP_HOST` stay explicit configuration (2.0.0).  It exists so an
 * operator can see which interfaces answer and what their individual and
 * multicast addresses are (`cometvisu-knx-fcgi --discover`).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cvknx {

struct DiscoveryOptions {
  /// Destination of the SEARCH_REQUEST (multicast group; unicast in tests).
  std::string address = "224.0.23.12";
  std::uint16_t port = 3671;
  /// Local bind port (0 = ephemeral).
  std::uint16_t bind_port = 0;
  /// Interface for the group join (empty = default).
  std::string iface;
  /// How long to collect SEARCH_RESPONSEs.
  int timeout_ms = 1000;
};

struct DiscoveredDevice {
  std::string name;
  std::uint16_t individual_address = 0;
  std::uint32_t multicast_address = 0;
  std::uint32_t address = 0;  ///< control endpoint, host byte order
  std::uint16_t port = 0;
};

/// Send one SEARCH_REQUEST and collect the answers for @p timeout_ms.
/// Never blocks past the deadline; returns an empty list when nothing answers.
[[nodiscard]] std::vector<DiscoveredDevice> discover_devices(const DiscoveryOptions& options);

}  // namespace cvknx
