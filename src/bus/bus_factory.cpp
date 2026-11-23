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
 * @file bus_factory.cpp
 * @brief Transport factory — the only place that knows the implementations.
 */

#include "bus/bus_factory.h"

#include <iostream>

#ifdef KNX_WITH_KNXD_TRANSPORT
#include "knxd/knxd_client.h"
#endif

#include "knxnetip/routing_bus_link.h"
#include "knxnetip/tunneling_bus_link.h"

namespace cvknx {

std::unique_ptr<BusLink> create_bus_link(const BusConfig& config) {
  // An if-chain instead of a switch: the knxd arm below is a compile-time
  // choice, so a switch would need a `default` (unreachable for a complete
  // enum) or leave `-Wreturn-type` unhappy.  A transport added to the enum and
  // forgotten here still fails loudly, because `bus_transport_to_string()`
  // switches over the same enum without a default.
  if (config.transport == BusTransport::IpTunnel) {
    return std::make_unique<TunnelingBusLink>(TunnelOptions::from_config(config));
  }
  if (config.transport == BusTransport::IpRouting) {
    return std::make_unique<RoutingBusLink>(RoutingOptions::from_config(config));
  }
  // knxd is the only transport that can be compiled out; the caller then falls
  // back to "bus unavailable" instead of failing at the first write.
#ifdef KNX_WITH_KNXD_TRANSPORT
  return std::make_unique<KnxdClient>(config.knxd_socket);
#else
  std::cerr << "[ERROR] KNX_TRANSPORT=knxd requires a build with "
               "-DKNX_BUILD_KNXD_TRANSPORT=ON\n";
  return nullptr;
#endif
}

}  // namespace cvknx
