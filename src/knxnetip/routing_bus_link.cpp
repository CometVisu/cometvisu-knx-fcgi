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
 * @file routing_bus_link.cpp
 * @brief KNXnet/IP IP routing transport implementation.
 *
 * Routing is stateless: there is no connection, no acknowledgement and no
 * keep-alive, and a write is published as a multicast `ROUTING_INDICATION`
 * that every other participant (including us, through the multicast loop)
 * receives.  That is why this link never reports a "bus" outage of its own —
 * a router that disappeared is indistinguishable from a quiet bus.
 */

#include "knxnetip/routing_bus_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

#include "knxnetip/cemi.h"
#include "knxnetip/knxnetip_frame.h"

namespace cvknx {
namespace {

constexpr std::uint8_t kMulticastTtl = 16;
constexpr int kReceiveBufferBytes = 2048;

/// IPv4 address of a network interface (host byte order).
/// @return std::nullopt when the interface does not exist or has no IPv4
///         address — both make it unusable for KNXnet/IP.
std::optional<std::uint32_t> interface_ipv4(const std::string& name) {
  if (name.size() >= IFNAMSIZ) {
    return std::nullopt;  // cannot be represented; never truncate silently
  }
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  // GCOV_EXCL_START - socket() only fails on fd exhaustion
  if (fd < 0) {
    return std::nullopt;
  }
  // GCOV_EXCL_STOP
  struct ifreq request {};
  std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const bool known = ::ioctl(fd, SIOCGIFADDR, &request) == 0;
  ::close(fd);
  if (!known) {
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* address = reinterpret_cast<const struct sockaddr_in*>(&request.ifr_addr);
  return ntohl(address->sin_addr.s_addr);
}

}  // namespace

RoutingOptions RoutingOptions::from_config(const BusConfig& config) {
  RoutingOptions options;
  options.address = config.ip_multicast;
  options.port = config.ip_port;
  options.iface = config.ip_iface;
  options.source_address = config.ip_individual_addr;
  options.bind_port = config.ip_port;
  return options;
}  // GCOV_EXCL_LINE - closing brace, no statement

RoutingBusLink::RoutingBusLink(const RoutingOptions& options) : options_(options) {}

RoutingBusLink::~RoutingBusLink() {
  disconnect();
}

bool RoutingBusLink::connect() {
  disconnect();

  const auto address = knxnetip::ipv4_from_string(options_.address);
  if (!address) {
    std::cerr << "[ERROR] Invalid KNX_IP_MULTICAST '" << options_.address << "'\n";
    return false;
  }

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  // GCOV_EXCL_START - socket() only fails on fd exhaustion
  if (fd_ < 0) {
    std::cerr << "[ERROR] Cannot create KNXnet/IP routing socket\n";
    return false;
  }
  // GCOV_EXCL_STOP

  int enable = 1;
  (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(options_.bind_port);
  if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
    std::cerr << "[ERROR] Cannot bind KNXnet/IP routing socket to port " << options_.bind_port
              << ": " << std::strerror(errno) << "\n";
    disconnect();
    return false;
  }

  struct sockaddr_in bound {};
  socklen_t bound_len = sizeof(bound);
  auto* bound_ptr = reinterpret_cast<struct sockaddr*>(&bound);
  // A socket that was just bound always answers; the failure arm is defensive.
  if (::getsockname(fd_, bound_ptr, &bound_len) == 0) {  // GCOV_EXCL_BR_LINE
    local_port_ = ntohs(bound.sin_port);
  }

  // Join the group only for a real multicast address.  A unicast address is
  // the documented loopback/test mode (no join is possible there).  The
  // address is in host byte order, which is what IN_MULTICAST() expects.
  std::optional<std::uint32_t> interface_address;
  if (!options_.iface.empty()) {
    interface_address = interface_ipv4(options_.iface);
    if (!interface_address) {
      std::cerr << "[ERROR] Unknown KNX_IP_IFACE '" << options_.iface
                << "' (or the interface has no IPv4 address)\n";
      disconnect();
      return false;
    }
  }

  const std::uint32_t host_address = *address;
  destination_ = host_address;
  if (IN_MULTICAST(host_address)) {
    struct ip_mreqn membership {};
    membership.imr_multiaddr.s_addr = htonl(host_address);
    membership.imr_address.s_addr =
        interface_address ? htonl(*interface_address) : htonl(INADDR_ANY);
    // A kernel that refuses the join is a host configuration problem that
    // cannot be injected from a test process.
    const bool joined =
        ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) == 0;
    // GCOV_EXCL_START - see above: only a misconfigured host reaches this
    if (!joined) {
      std::cerr << "[ERROR] Cannot join KNXnet/IP multicast group " << options_.address << "\n";
      disconnect();
      return false;
    }
    // GCOV_EXCL_STOP
  }

  // Ask the kernel for the destination address of every datagram (IP_PKTINFO):
  // that is the only way to tell a KNX routing indication addressed to our
  // group from a unicast datagram sprayed at our port by an unrelated host.
  int pktinfo = 1;
  (void)::setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &pktinfo, sizeof(pktinfo));

  // Select the outgoing interface by its address.  An ip_mreqn that carries
  // only an interface index leaves the source address to the routing table,
  // which on a multi-homed host (and when another KNXnet/IP participant runs
  // on the same host) is the wrong one.
  if (interface_address) {
    struct in_addr outgoing {};
    outgoing.s_addr = htonl(*interface_address);
    (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, sizeof(outgoing));
  }

  unsigned char ttl = kMulticastTtl;
  (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  // Loop enabled: a local router (or knxd in ip: mode) must see our frames,
  // and our own frame coming back is the bus echo of the write (see the class
  // comment).
  unsigned char loop = 1;
  (void)::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  // Receive loops call recv() until EAGAIN — never block there.
  set_nonblocking(true);
  return true;
}

void RoutingBusLink::disconnect() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  local_port_ = 0;
  destination_ = 0;
}

bool RoutingBusLink::send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) {
  if (fd_ < 0) {
    return false;
  }

  const auto cemi = cemi::encode_group_indication(options_.source_address, group_addr, apdu);
  if (cemi.empty()) {
    return false;
  }
  const auto datagram = knxnetip::build_routing_indication(cemi);

  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(destination_);
  destination.sin_port = htons(options_.port);

  const ssize_t sent =
      ::sendto(fd_, datagram.data(), datagram.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
  if (sent != static_cast<ssize_t>(datagram.size())) {
    // Routing is fire-and-forget, but a broken socket (ENETUNREACH, ENODEV)
    // must surface as a failed write so /w answers 503.
    std::cerr << "[WARN] KNXnet/IP routing send failed: " << std::strerror(errno) << "\n";
    return false;
  }
  return true;
}

std::optional<std::uint32_t> RoutingBusLink::destination_of(struct msghdr& message) {
  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&message, cmsg)) {
    // Other control messages (timestamps, TTL, ...) may be present.
    if (cmsg->cmsg_level != IPPROTO_IP || cmsg->cmsg_type != IP_PKTINFO ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(struct in_pktinfo))) {
      continue;
    }
    struct in_pktinfo info {};
    std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
    return ntohl(info.ipi_addr.s_addr);
  }
  return std::nullopt;
}

bool RoutingBusLink::poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) {
  if (fd_ < 0) {
    return false;
  }

  std::uint8_t buffer[kReceiveBufferBytes];  // NOLINT(modernize-avoid-c-arrays)
  for (;;) {
    struct iovec iov {};
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(struct in_pktinfo))] = {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t received = ::recvmsg(fd_, &message, 0);
    if (received <= 0) {
      return false;  // EAGAIN or error — nothing (more) to read
    }
    // A truncated control block cannot prove where the datagram was sent.
    if ((message.msg_flags & MSG_CTRUNC) != 0) {  // GCOV_EXCL_BR_LINE - only IP_PKTINFO is enabled
      continue;                                   // GCOV_EXCL_LINE
    }
    const auto addressed_to = destination_of(message);
    // GCOV_EXCL_START - a kernel that ignored IP_PKTINFO is a host
    // configuration problem and cannot be injected from a test
    if (!addressed_to) {
      continue;  // cannot be verified → drop
    }
    // GCOV_EXCL_STOP
    if (*addressed_to != destination_) {
      continue;  // sent to some other address, not to the KNX routing group
    }
    const std::span<const std::uint8_t> datagram(buffer, static_cast<std::size_t>(received));
    const auto cemi_payload = knxnetip::parse_routing_payload(datagram);
    if (!cemi_payload) {
      continue;  // other service types are ignored
    }
    const auto frame = cemi::parse_ldata_ind(*cemi_payload);
    if (!frame) {
      continue;  // L_Data.req / L_Data.con / individual traffic
    }
    out_group_addr = frame->destination;
    out_apdu = frame->apdu;
    ++telegram_count_;
    return true;
  }
}

void RoutingBusLink::set_nonblocking(bool enable) {
  if (fd_ < 0) {
    return;
  }
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {  // GCOV_EXCL_BR_LINE - fcntl on a valid fd cannot fail here
    return;         // GCOV_EXCL_LINE
  }
  ::fcntl(fd_, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

BusLink::WaitResult RoutingBusLink::wait_for_activity(int timeout_ms) {
  if (fd_ < 0) {
    return WaitResult::Error;
  }
  struct pollfd pfd {};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc == 0) {
    return WaitResult::Timeout;
  }
  if (rc < 0) {  // GCOV_EXCL_BR_LINE - a UDP socket without signals does not fail poll()
    return errno == EINTR ? WaitResult::Timeout : WaitResult::Error;  // GCOV_EXCL_LINE
  }
  if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {  // GCOV_EXCL_BR_LINE
    return WaitResult::Error;                                 // GCOV_EXCL_LINE
  }
  if ((pfd.revents & POLLIN) != 0) {  // GCOV_EXCL_BR_LINE - poll() only reports POLLIN here
    return WaitResult::GroupData;
  }
  return WaitResult::Timeout;  // GCOV_EXCL_LINE - every revents combination is handled above
}

std::string RoutingBusLink::label() const {
  return "KNXnet/IP routing " + options_.address + ":" + std::to_string(options_.port);
}

}  // namespace cvknx
