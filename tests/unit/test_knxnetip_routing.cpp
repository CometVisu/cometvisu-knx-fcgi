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
 * @file test_knxnetip_routing.cpp
 * @brief IP routing link against a loopback UDP peer (never port 3671, never
 *        multicast — CI must not depend on it).
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/knxnetip/cemi.h"
#include "../../src/knxnetip/knxnetip_frame.h"
#include "../../src/knxnetip/routing_bus_link.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

/// A loopback UDP endpoint that plays the role of the KNX router.
class UdpPeer {
public:
  UdpPeer() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
      return;
    }
    socklen_t len = sizeof(address);
    (void)::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&address), &len);
    port_ = ntohs(address.sin_port);
    struct timeval timeout {
      2, 0
    };
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  ~UdpPeer() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  UdpPeer(const UdpPeer&) = delete;
  UdpPeer& operator=(const UdpPeer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

  Bytes receive() {
    std::uint8_t buffer[2048] = {};
    const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return {};
    }
    return Bytes(buffer, buffer + received);
  }

  void send_to(std::uint16_t port, const Bytes& data) {
    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);
    (void)::sendto(fd_, data.data(), data.size(), 0,
                   reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
  }

private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
};

RoutingOptions LoopbackOptions(std::uint16_t destination_port) {
  RoutingOptions options;
  options.address = "127.0.0.1";
  options.port = destination_port;
  options.bind_port = 0;  // ephemeral — the peer owns the destination port
  return options;
}

Bytes Indication(uint16_t group_addr, const Bytes& apdu, std::uint8_t message_code) {
  Bytes cemi = cemi::encode_group_request(0x1101, group_addr, apdu);
  EXPECT_FALSE(cemi.empty());
  cemi[0] = message_code;
  return knxnetip::build_routing_indication(cemi);
}

TEST(RoutingBusLinkTest, SendsRoutingIndicationWithCemi) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  ASSERT_TRUE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));

  const auto received = peer.receive();
  // A routing participant is a router: the cEMI must be an L_Data.ind with its
  // own individual address.  An L_Data.req is dropped by every real router.
  const Bytes expected = knxnetip::build_routing_indication(
      cemi::encode_group_indication(0xFFFA, 0x0A03, cemi::Bytes{0x00, 0x80, 0x42}));
  EXPECT_EQ(received, expected);
  ASSERT_FALSE(received.empty());
  EXPECT_EQ(received[6], cemi::kMessageCodeLDataInd);
  EXPECT_EQ(received[10], 0xFF);  // source 15.15.250 -> 0xFFFA
  EXPECT_EQ(received[11], 0xFA);
}

TEST(RoutingBusLinkTest, UsesTheConfiguredSourceAddress) {
  UdpPeer peer;
  RoutingOptions options = LoopbackOptions(peer.port());
  options.source_address = 0x11F0;  // 1.1.240
  RoutingBusLink link(options);
  ASSERT_TRUE(link.connect());

  ASSERT_TRUE(link.send_group_packet(0x0A04, {0x00, 0x80, 0x01}));

  const auto received = peer.receive();
  ASSERT_GE(received.size(), 12U);
  EXPECT_EQ(received[10], 0x11);
  EXPECT_EQ(received[11], 0xF0);
}

TEST(RoutingBusLinkTest, FromConfigTakesTheIndividualAddress) {
  BusConfig config;
  config.transport = BusTransport::IpRouting;
  config.ip_multicast = "239.255.1.2";
  config.ip_port = 1234;
  config.ip_individual_addr = 0x11F0;
  const RoutingOptions options = RoutingOptions::from_config(config);
  EXPECT_EQ(options.address, "239.255.1.2");
  EXPECT_EQ(options.port, 1234);
  EXPECT_EQ(options.bind_port, 1234);
  EXPECT_EQ(options.source_address, 0x11F0);
}

TEST(RoutingBusLinkTest, FramesLeaveThroughTheConfiguredInterface) {
  // The outgoing interface must be selected by its *address*, not by its
  // index: an ip_mreqn with only an index lets the routing table pick the
  // source address, which is wrong when several KNXnet/IP participants share
  // a host.  A group member on "lo" sees the frame only with source 127.0.0.1.
  const std::string group = "239.255.99.222";
  const int receiver = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(receiver, 0);
  int enable = 1;
  (void)::setsockopt(receiver, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  struct sockaddr_in bind_address {};
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = 0;
  ASSERT_EQ(
      ::bind(receiver, reinterpret_cast<struct sockaddr*>(&bind_address), sizeof(bind_address)), 0);
  socklen_t length = sizeof(bind_address);
  ASSERT_EQ(::getsockname(receiver, reinterpret_cast<struct sockaddr*>(&bind_address), &length), 0);
  const std::uint16_t port = ntohs(bind_address.sin_port);
  struct ip_mreqn membership {};
  membership.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
  membership.imr_address.s_addr = ::inet_addr("127.0.0.1");
  ASSERT_EQ(::setsockopt(receiver, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)),
            0);

  RoutingOptions options;
  options.address = group;
  options.port = port;
  options.bind_port = port;
  options.iface = "lo";
  RoutingBusLink link(options);
  ASSERT_TRUE(link.connect());
  ASSERT_TRUE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));

  struct sockaddr_in from {};
  socklen_t from_len = sizeof(from);
  std::uint8_t buffer[256] = {};
  const ssize_t received = ::recvfrom(receiver, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<struct sockaddr*>(&from), &from_len);
  EXPECT_GT(received, 0);
  EXPECT_EQ(std::string{::inet_ntoa(from.sin_addr)}, "127.0.0.1");
  EXPECT_EQ(ntohs(from.sin_port), port);
  ::close(receiver);
}

TEST(RoutingBusLinkTest, UnknownInterfaceIsRejected) {
  RoutingOptions options;
  options.address = "239.255.99.223";
  options.iface = "definitely-not-an-interface0";
  RoutingBusLink link(options);
  EXPECT_FALSE(link.connect());
  EXPECT_FALSE(link.is_connected());
}

TEST(RoutingBusLinkTest, ReceivesRoutingIndication) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());
  ASSERT_NE(link.local_port(), 0);

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x0C, 0x6F}, 0x29));

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));
  EXPECT_EQ(link.get_telegram_count(), 1U);
}

TEST(RoutingBusLinkTest, IgnoresRequestFrames) {
  // L_Data.req (0x11) is a *confirmation* path frame; only indications are bus
  // traffic.  A frame that exists only as .req must not be delivered.
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x42}, 0x11));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(RoutingBusLinkTest, IgnoresConfirmations) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  peer.send_to(link.local_port(), Indication(0x0A03, {0x00, 0x80, 0x42}, 0x2E));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

TEST(RoutingBusLinkTest, IgnoresOtherServiceTypes) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  const Bytes search = knxnetip::build_search_request(knxnetip::Hpai{});
  peer.send_to(link.local_port(), search);

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
}

/// A routing participant cannot tell senders apart, but it can verify that a
/// datagram was actually addressed to the KNX routing destination.  A unicast
/// packet sprayed at the port by an unrelated host must be dropped, not parsed
/// as bus traffic (IP_PKTINFO check).
TEST(RoutingBusLinkTest, IgnoresDatagramsNotAddressedToTheRoutingGroup) {
  UdpPeer peer;
  RoutingOptions options;  // a real multicast group
  options.port = peer.port();
  options.bind_port = 0;
  RoutingBusLink link(options);
  if (!link.connect()) {
    GTEST_SKIP() << "this host cannot join the KNXnet/IP routing group";
  }

  // Unicast to 127.0.0.1:<port> — the destination address of this datagram is
  // not the routing group, so it must not reach the cache.
  peer.send_to(link.local_port(),
               Indication(0x0A03, Bytes{0x00, 0x80, 0x01}, cemi::kMessageCodeLDataInd));

  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu))
      << "a datagram addressed elsewhere was parsed as bus traffic";
  EXPECT_EQ(link.get_telegram_count(), 0U);
}

/// Destination extraction from the control messages of a received datagram.
TEST(RoutingBusLinkDestination, ReturnsThePktinfoAddress) {
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(struct in_pktinfo))] = {};
  struct msghdr message {};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
  ASSERT_NE(cmsg, nullptr);
  cmsg->cmsg_level = IPPROTO_IP;
  cmsg->cmsg_type = IP_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
  struct in_pktinfo info {};
  info.ipi_addr.s_addr = htonl(0xE000170CU);  // 224.0.23.12
  std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));

  const auto destination = RoutingBusLink::destination_of(message);
  ASSERT_TRUE(destination.has_value());
  EXPECT_EQ(*destination, 0xE000170CU);
}

/// A datagram without destination information cannot be verified.
TEST(RoutingBusLinkDestination, ReportsNoDestinationWithoutPktinfo) {
  struct msghdr message {};
  EXPECT_FALSE(RoutingBusLink::destination_of(message).has_value());

  // A different control message (e.g. a timestamp) is skipped.
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(struct timeval))] = {};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
  ASSERT_NE(cmsg, nullptr);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMP;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct timeval));
  EXPECT_FALSE(RoutingBusLink::destination_of(message).has_value());
}

/// A different IP-level control message is skipped as well.
TEST(RoutingBusLinkDestination, SkipsNonPktinfoIpControlMessages) {
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  struct msghdr message {};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
  ASSERT_NE(cmsg, nullptr);
  cmsg->cmsg_level = IPPROTO_IP;
  cmsg->cmsg_type = IP_TTL;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  EXPECT_FALSE(RoutingBusLink::destination_of(message).has_value());
}

/// A truncated IP_PKTINFO (a kernel bug) must not be read.
TEST(RoutingBusLinkDestination, IgnoresATruncatedPktinfo) {
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(struct in_pktinfo))] = {};
  struct msghdr message {};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
  ASSERT_NE(cmsg, nullptr);
  cmsg->cmsg_level = IPPROTO_IP;
  cmsg->cmsg_type = IP_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo)) - 1;  // too short
  EXPECT_FALSE(RoutingBusLink::destination_of(message).has_value());
}

TEST(RoutingBusLinkTest, SendFailsWhenNotConnected) {
  RoutingBusLink link(LoopbackOptions(3671));
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(link.wait_for_activity(1), BusLink::WaitResult::Error);

  ASSERT_TRUE(link.connect());
  link.disconnect();
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
}

TEST(RoutingBusLinkTest, EmptyApduRejected) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());
  EXPECT_FALSE(link.send_group_packet(0x0A03, {}));
}

TEST(RoutingBusLinkTest, DoesNotSpinWhenSocketSilent) {
  UdpPeer peer;
  RoutingBusLink link(LoopbackOptions(peer.port()));
  ASSERT_TRUE(link.connect());

  const auto start = std::chrono::steady_clock::now();
  const auto result = link.wait_for_activity(200);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  EXPECT_EQ(result, BusLink::WaitResult::Timeout);
  EXPECT_GE(elapsed, 150);
  EXPECT_LT(elapsed, 1000);
}

TEST(RoutingBusLinkTest, LabelNamesTheGroup) {
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.port = 3671;
  RoutingBusLink link(options);
  EXPECT_NE(link.label().find("224.0.23.12:3671"), std::string::npos);
}

}  // namespace
}  // namespace cvknx
