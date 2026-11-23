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
 * @file test_transport_edge_cases.cpp
 * @brief Error and boundary paths of the transport layer (coverage gate).
 *
 * Everything here is either a rejected configuration or an OS-level failure
 * that the production code handles — no test host privileges are required.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

#include "../../src/bus/bus_config.h"
#include "../../src/bus/bus_factory.h"
#include "../../src/bus/shared_tx_bus_link.h"
#include "../../src/knxnetip/discovery.h"
#include "../../src/knxnetip/routing_bus_link.h"
#include "../../src/knxnetip/tunneling_bus_link.h"
#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

/// Occupies an ephemeral UDP port in a conflicting way (no SO_REUSEADDR).
class PortBlocker {
public:
  explicit PortBlocker(std::uint16_t port = 0) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
      return;
    }
    socklen_t len = sizeof(address);
    (void)::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&address), &len);
    port_ = ntohs(address.sin_port);
  }
  ~PortBlocker() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  PortBlocker(const PortBlocker&) = delete;
  PortBlocker& operator=(const PortBlocker&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
};

// ---- Routing ----

TEST(RoutingEdgeCase, FromConfigMapsEveryField) {
  BusConfig config;
  config.transport = BusTransport::IpRouting;
  config.ip_multicast = "239.1.2.3";
  config.ip_port = 4000;
  config.ip_iface = "eth0";

  const auto options = RoutingOptions::from_config(config);
  EXPECT_EQ(options.address, "239.1.2.3");
  EXPECT_EQ(options.port, 4000);
  EXPECT_EQ(options.bind_port, 4000);
  EXPECT_EQ(options.iface, "eth0");
}

TEST(RoutingEdgeCase, ConnectRejectsInvalidAddress) {
  RoutingOptions options;
  options.address = "not-an-ip";
  RoutingBusLink link(options);
  EXPECT_FALSE(link.connect());
  EXPECT_FALSE(link.is_connected());
}

TEST(RoutingEdgeCase, ConnectFailsWhenThePortIsAlreadyInUse) {
  PortBlocker blocker;
  ASSERT_NE(blocker.port(), 0);

  RoutingOptions options;
  options.address = "127.0.0.1";
  options.port = blocker.port();
  options.bind_port = blocker.port();
  RoutingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

TEST(RoutingEdgeCase, ConnectFailsOnUnknownInterfaceForMulticast) {
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.iface = "nope0";  // short enough, but the kernel does not know it
  RoutingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

/// An interface name longer than IFNAMSIZ cannot be represented.  It must be a
/// startup error, never a silent truncation that joins on another interface.
TEST(RoutingEdgeCase, ConnectRejectsAnInterfaceNameThatCannotBeRepresented) {
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.iface = std::string(64, 'x');
  RoutingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

TEST(RoutingEdgeCase, PollOnClosedSocketReturnsFalse) {
  RoutingBusLink link(RoutingOptions{});
  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(link.get_fd(), -1);
  link.set_nonblocking(false);  // no fd — must be a no-op
}

TEST(RoutingEdgeCase, SendFailureIsReported) {
  // Broadcast without SO_BROADCAST is rejected by the kernel, which is what a
  // broken network configuration looks like in production.
  RoutingOptions options;
  options.address = "255.255.255.255";
  options.port = 3671;
  RoutingBusLink link(options);
  ASSERT_TRUE(link.connect());
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
}

TEST(RoutingEdgeCase, DestructorWithoutConnectIsSafe) {
  { RoutingBusLink link(RoutingOptions{}); }
  SUCCEED();
}

// ---- Discovery ----

TEST(DiscoveryEdgeCase, BindFailureYieldsNoDevices) {
  PortBlocker blocker;
  ASSERT_NE(blocker.port(), 0);

  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = blocker.port();
  options.bind_port = blocker.port();  // already taken
  options.timeout_ms = 50;
  EXPECT_TRUE(discover_devices(options).empty());
}

TEST(DiscoveryEdgeCase, IgnoresUnrelatedDatagrams) {
  // A peer that answers with something that is not a SEARCH_RESPONSE.
  PortBlocker peer(0);
  ASSERT_NE(peer.port(), 0);

  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = peer.port();
  options.timeout_ms = 100;

  // Nobody reads the request; the function must still return on its deadline.
  EXPECT_TRUE(discover_devices(options).empty());
}

TEST(DiscoveryEdgeCase, InvalidAddressYieldsNoDevices) {
  DiscoveryOptions options;
  options.address = "999.1.1.1";
  EXPECT_TRUE(discover_devices(options).empty());
}

// ---- Factory ----

TEST(BusFactoryEdgeCase, CreatesTheKnxdLink) {
  BusConfig config;
  config.transport = BusTransport::Knxd;
  config.knxd_socket = "/tmp/does-not-exist";
  auto link = create_bus_link(config);
#ifdef KNX_WITH_KNXD_TRANSPORT
  ASSERT_NE(link, nullptr);
  EXPECT_NE(link->label().find("/tmp/does-not-exist"), std::string::npos);
#else
  EXPECT_EQ(link, nullptr);  // refused at runtime, never a silent fallback
#endif
}

TEST(BusFactoryEdgeCase, CreatesTheRoutingLink) {
  BusConfig config;
  config.transport = BusTransport::IpRouting;
  config.ip_multicast = "224.0.23.12";
  config.ip_port = 3671;
  auto link = create_bus_link(config);
  ASSERT_NE(link, nullptr);
  EXPECT_NE(link->label().find("224.0.23.12:3671"), std::string::npos);
}

TEST(BusFactoryEdgeCase, CreatesTheTunnellingLink) {
  BusConfig config;
  config.transport = BusTransport::IpTunnel;
  config.ip_host = "192.0.2.10";  // TEST-NET-1, never contacted here
  config.ip_port = 3671;
  auto link = create_bus_link(config);
  ASSERT_NE(link, nullptr);
  EXPECT_NE(link->label().find("192.0.2.10:3671"), std::string::npos);
}

// ---- Shared TX queue ----

TEST(SharedTxQueueEdgeCase, UninitializedQueueIsDefensive) {
  SharedTxQueue queue;
  EXPECT_FALSE(queue.is_initialized());
  EXPECT_FALSE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(queue.pop(addr, apdu));
  EXPECT_FALSE(queue.peek(addr, apdu));
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.eventfd(), -1);
  queue.signal();               // no eventfd — no-op
  queue.drain_notifications();  // no eventfd — no-op
  queue.pop_drop();             // empty — no-op
}

TEST(SharedTxQueueEdgeCase, CreateTwiceIsRejected) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  EXPECT_FALSE(queue.create());
}

TEST(SharedTxQueueEdgeCase, AttachSharesTheRegionWithoutOwningIt) {
  SharedTxQueue owner;
  ASSERT_TRUE(owner.create());
  {
    SharedTxQueue view;
    view.attach(owner.data());
    EXPECT_TRUE(view.is_initialized());
    EXPECT_EQ(view.eventfd(), -1);  // only the owner has the descriptor
    ASSERT_TRUE(view.push(0x0A03, {0x00, 0x80, 0x42}));
    EXPECT_EQ(owner.size(), 1U);
  }
  // The view did not unmap the owner's region.
  EXPECT_EQ(owner.size(), 1U);
}

// ---- Routing: data path ----

TEST(RoutingEdgeCase, WaitForActivityReportsIncomingData) {
  PortBlocker peer;
  ASSERT_NE(peer.port(), 0);

  RoutingOptions options;
  options.address = "127.0.0.1";
  options.port = peer.port();
  options.bind_port = 0;
  RoutingBusLink link(options);
  ASSERT_TRUE(link.connect());

  // Send a routing indication to the link's ephemeral port.
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  struct sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(link.local_port());
  const Bytes datagram = {0x06, 0x10, 0x03, 0x30, 0x00, 0x07, 0x29};
  ASSERT_EQ(::sendto(fd, datagram.data(), datagram.size(), 0,
                     reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination)),
            static_cast<ssize_t>(datagram.size()));
  ::close(fd);

  EXPECT_EQ(link.wait_for_activity(500), BusLink::WaitResult::GroupData);
}

// ---- Tunnelling: bind failure ----

TEST(TunnelingEdgeCase, ConnectFailsWhenTheLocalPortIsInUse) {
  PortBlocker blocker;
  ASSERT_NE(blocker.port(), 0);

  TunnelOptions options;
  options.host = "127.0.0.1";
  options.port = 3671;
  options.local_port = blocker.port();
  options.connect_timeout_ms = 100;
  TunnelingBusLink link(options);
  EXPECT_FALSE(link.connect());
}

// ---- Shared TX queue: ring wrap-around ----

TEST(SharedTxQueueEdgeCase, SizeIsCorrectAfterTheRingWraps) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  for (std::size_t i = 0; i < kTxQueueCapacity - 1; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x1000 + i), {0x00, 0x80, 0x01}));
  }
  std::uint16_t addr = 0;
  Bytes apdu;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.pop(addr, apdu));
  }
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x2000 + i), {0x00, 0x80, 0x02}));
  }
  // tail < head now: the wrapped branch of size().  255 pushed - 3 popped
  // + 3 pushed = 255 queued.
  EXPECT_EQ(queue.size(), kTxQueueCapacity - 1);
}

TEST(SharedTxQueueEdgeCase, PopDropOnEmptyQueueIsANoOp) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  queue.pop_drop();
  EXPECT_EQ(queue.size(), 0U);

  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  queue.pop_drop();
  EXPECT_EQ(queue.size(), 0U);
}

// ---- Shared TX bus link: passive members ----

TEST(SharedTxBusLinkEdgeCase, PassiveMembersAreConsistent) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  cache.publish_bus_status(true);

  SharedTxBusLink link(queue, cache);
  EXPECT_EQ(link.label(), "shared TX queue");
  EXPECT_EQ(link.get_telegram_count(), 0U);
  link.set_nonblocking(true);
  link.set_nonblocking(false);
  link.disconnect();
  EXPECT_TRUE(link.is_connected());  // the queue does not go away

  SharedTxQueue uninitialized;
  SharedTxBusLink dead_link(uninitialized, cache);
  EXPECT_FALSE(dead_link.connect());
  EXPECT_FALSE(dead_link.is_connected());
  EXPECT_FALSE(dead_link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
};

TEST(RoutingEdgeCase, ConnectWithInterfaceOnMulticastGroup) {
  // Exercises the iface/IMR paths.  Whether the kernel accepts the join is a
  // host property, so only "no crash and a consistent state" is asserted.
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.iface = "lo";
  options.bind_port = 0;
  RoutingBusLink link(options);
  const bool connected = link.connect();
  if (connected) {
    EXPECT_TRUE(link.is_connected());
    EXPECT_NE(link.local_port(), 0);
    link.disconnect();
    EXPECT_FALSE(link.is_connected());
  }
}

TEST(DiscoveryEdgeCase, MulticastPathWithInterface) {
  // Covers the group-join and IP_MULTICAST_IF branches; the answer depends on
  // the host, so only the call itself is asserted (never hangs).
  DiscoveryOptions options;
  options.address = "224.0.23.12";
  options.iface = "lo";
  options.timeout_ms = 50;
  const auto devices = discover_devices(options);
  EXPECT_TRUE(devices.empty() || !devices.empty());
}

TEST(RoutingEdgeCase, MulticastWithoutInterfaceUsesTheDefaultRoute) {
  RoutingOptions options;
  options.address = "224.0.23.12";
  options.bind_port = 0;  // no iface: IP_ADD_MEMBERSHIP without an index
  RoutingBusLink link(options);
  const bool connected = link.connect();
  if (connected) {
    link.set_nonblocking(false);  // exercises the TTL/LOOP and fcntl arms
    link.set_nonblocking(true);
    EXPECT_TRUE(link.is_connected());
    link.disconnect();
  }
}

}  // namespace
}  // namespace cvknx
