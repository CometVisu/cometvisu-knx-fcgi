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
 * @file discovery.cpp
 * @brief KNXnet/IP discovery implementation (`--discover`).
 *
 * A `SEARCH_REQUEST` is sent as a multicast (or unicast, if configured)
 * datagram with HPAI 0.0.0.0:0, so every device answers to the source address
 * of our datagram — the interface does not have to be known in advance, which
 * is what makes the command usable on a machine with several NICs.
 *
 * The function is synchronous and bounded by `timeout_ms`: it collects answers
 * until the deadline expires, because a KNXnet/IP backbone has no "end of
 * answers" marker and a second of waiting is cheap compared to a rerun.
 */

#include "knxnetip/discovery.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "knxnetip/knxnetip_frame.h"

namespace cvknx {
namespace {

constexpr int kReceiveBufferBytes = 2048;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::vector<DiscoveredDevice> discover_devices(const DiscoveryOptions& options) {
  std::vector<DiscoveredDevice> devices;

  const auto destination_address = knxnetip::ipv4_from_string(options.address);
  if (!destination_address) {
    return devices;
  }

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {      // GCOV_EXCL_BR_LINE - only fails on fd exhaustion
    return devices;  // GCOV_EXCL_LINE
  }
  int enable = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(options.bind_port);
  // Bind before sending: the answers go to the source address of the request,
  // so the socket has to exist and be bound first.  bind_port 0 lets the OS
  // pick an ephemeral port.
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
    ::close(fd);
    return devices;
  }

  // Host byte order — that is what IN_MULTICAST() expects.
  if (IN_MULTICAST(*destination_address)) {
    // Join the group so the socket receives the answers that are sent back to
    // the multicast group as well (a KNXnet/IP router may answer to the group
    // rather than to our unicast source address).
    struct ip_mreqn membership {};
    membership.imr_multiaddr.s_addr = htonl(*destination_address);
    membership.imr_address.s_addr = htonl(INADDR_ANY);
    if (!options.iface.empty()) {
      const unsigned index = ::if_nametoindex(options.iface.c_str());
      if (index != 0) {
        membership.imr_ifindex = static_cast<int>(index);
      }
    }
    (void)::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
  }

  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(*destination_address);
  destination.sin_port = htons(options.port);

  const auto request = knxnetip::build_search_request(knxnetip::Hpai{0U, 0U});
  if (::sendto(fd, request.data(), request.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&destination),
               sizeof(destination)) != static_cast<ssize_t>(request.size())) {  // GCOV_EXCL_BR_LINE
    // A local sendto() on a freshly bound socket does not fail in practice.
    ::close(fd);     // GCOV_EXCL_LINE
    return devices;  // GCOV_EXCL_LINE
  }

  const std::int64_t deadline = now_ms() + std::max(1, options.timeout_ms);
  while (now_ms() < deadline) {  // GCOV_EXCL_BR_LINE - every exit goes through break
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int remaining = static_cast<int>(deadline - now_ms());
    if (::poll(&pfd, 1, std::max(1, remaining)) <= 0) {
      break;
    }
    std::uint8_t buffer[kReceiveBufferBytes];  // NOLINT(modernize-avoid-c-arrays)
    struct sockaddr_in from {};
    socklen_t from_len = sizeof(from);
    const ssize_t received = ::recvfrom(fd, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<struct sockaddr*>(&from), &from_len);
    if (received <= 0) {  // GCOV_EXCL_BR_LINE - poll() said readable
      continue;           // GCOV_EXCL_LINE
    }
    const std::vector<std::uint8_t> datagram(buffer,
                                             buffer + static_cast<std::ptrdiff_t>(received));
    const auto response = knxnetip::parse_search_response(datagram);
    if (!response) {
      continue;
    }
    DiscoveredDevice device;
    device.name = response->device_name;
    device.individual_address = response->individual_address;
    device.multicast_address = response->multicast_address;
    device.address = response->endpoint.address;
    device.port = response->endpoint.port;
    // One entry per endpoint, even when a router answers on several interfaces.
    const bool known = std::any_of(devices.begin(), devices.end(), [&device](const auto& entry) {
      if (entry.address != device.address) {
        return false;
      }
      return entry.port == device.port;
    });
    if (!known) {
      devices.push_back(device);
    }
  }

  ::close(fd);
  return devices;
}

}  // namespace cvknx
