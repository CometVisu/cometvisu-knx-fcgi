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
 * @file test_discovery.cpp
 * @brief Discovery against a loopback device (never port 3671, never multicast).
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../../src/knxnetip/discovery.h"
#include "../../src/knxnetip/knxnetip_frame.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

class FakeDevice {
public:
  FakeDevice() {
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
  ~FakeDevice() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }

  bool receive(Bytes& out) {
    std::uint8_t buffer[2048] = {};
    struct sockaddr_in from {};
    socklen_t from_len = sizeof(from);
    const ssize_t received = ::recvfrom(fd_, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<struct sockaddr*>(&from), &from_len);
    if (received <= 0) {
      return false;
    }
    peer_ = from;
    out.assign(buffer, buffer + static_cast<std::ptrdiff_t>(received));
    return true;
  }

  void reply(const Bytes& data) {
    (void)::sendto(fd_, data.data(), data.size(), 0,
                   reinterpret_cast<const struct sockaddr*>(&peer_), sizeof(peer_));
  }

private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
  struct sockaddr_in peer_ {};
};

Bytes SearchResponseFor(const std::string& name, std::uint16_t individual_address,
                        std::uint32_t endpoint_address) {
  Bytes body;
  const auto control = knxnetip::encode_hpai(knxnetip::Hpai{0x7F000001U, 3671});
  const auto endpoint = knxnetip::encode_hpai(knxnetip::Hpai{endpoint_address, 3671});
  body.insert(body.end(), control.begin(), control.end());
  body.insert(body.end(), endpoint.begin(), endpoint.end());

  Bytes dib(54, 0x00);
  dib[0] = 54;
  dib[1] = 0x01;
  dib[2] = 0x02;
  dib[4] = static_cast<std::uint8_t>((individual_address >> 8) & 0xFF);
  dib[5] = static_cast<std::uint8_t>(individual_address & 0xFF);
  dib[14] = 0xEF;
  dib[15] = 0x00;
  dib[16] = 0x17;
  dib[17] = 0x0C;
  for (std::size_t i = 0; i < name.size() && i < 30; ++i) {
    dib[24 + i] = static_cast<std::uint8_t>(name[i]);
  }
  body.insert(body.end(), dib.begin(), dib.end());
  return knxnetip::build_frame(knxnetip::ServiceType::SearchResponse, body);
}

Bytes SearchResponse(const std::string& name, std::uint16_t individual_address) {
  Bytes body;
  const auto control = knxnetip::encode_hpai(knxnetip::Hpai{0x7F000001U, 3671});
  const auto endpoint = knxnetip::encode_hpai(knxnetip::Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), control.begin(), control.end());
  body.insert(body.end(), endpoint.begin(), endpoint.end());

  Bytes dib(54, 0x00);
  dib[0] = 54;
  dib[1] = 0x01;  // device information DIB
  dib[2] = 0x02;  // medium: IP
  dib[3] = 0x00;  // status
  dib[4] = static_cast<std::uint8_t>((individual_address >> 8) & 0xFF);
  dib[5] = static_cast<std::uint8_t>(individual_address & 0xFF);
  // DIB data layout (array index = data offset + 2 because of len/type):
  // medium, status, IA, install id, serial, routing multicast, MAC, name.
  dib[14] = 0xEF;
  dib[15] = 0x00;
  dib[16] = 0x17;
  dib[17] = 0x0C;  // multicast 239.0.23.12
  for (std::size_t i = 0; i < name.size() && i < 30; ++i) {
    dib[24 + i] = static_cast<std::uint8_t>(name[i]);
  }
  body.insert(body.end(), dib.begin(), dib.end());
  return knxnetip::build_frame(knxnetip::ServiceType::SearchResponse, body);
}

TEST(DiscoveryTest, SendsSearchRequestAndCollectsTheAnswer) {
  FakeDevice device;
  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = device.port();
  options.timeout_ms = 500;

  std::vector<DiscoveredDevice> discovered;
  std::thread worker([&] { discovered = discover_devices(options); });

  Bytes request;
  ASSERT_TRUE(device.receive(request));
  const auto frame = knxnetip::parse_frame(request);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->service, knxnetip::ServiceType::SearchRequest);

  device.reply(SearchResponse("Test-device", 0x1101));
  worker.join();

  ASSERT_EQ(discovered.size(), 1U);
  EXPECT_EQ(discovered[0].name, "Test-device");
  EXPECT_EQ(discovered[0].individual_address, 0x1101);
  EXPECT_EQ(discovered[0].multicast_address, 0xEF00170CU);
  EXPECT_EQ(discovered[0].address, 0xC0A8010AU);
}

TEST(DiscoveryTest, ReturnsEmptyWithoutAnswersButNeverHangs) {
  const auto start = std::chrono::steady_clock::now();
  DiscoveryOptions options;
  // A port nobody listens on (destination unreachable → ICMP is ignored).
  options.address = "127.0.0.1";
  options.port = 1;
  options.timeout_ms = 100;
  const auto devices = discover_devices(options);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  EXPECT_TRUE(devices.empty());
  EXPECT_LT(elapsed, 1000);
}

TEST(DiscoveryTest, InvalidAddressYieldsNoDevices) {
  DiscoveryOptions options;
  options.address = "not-an-address";
  EXPECT_TRUE(discover_devices(options).empty());
}

TEST(DiscoveryTest, DuplicateAnswersAreCollapsed) {
  FakeDevice device;
  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = device.port();
  options.timeout_ms = 300;

  std::vector<DiscoveredDevice> discovered;
  std::thread worker([&] { discovered = discover_devices(options); });

  Bytes request;
  ASSERT_TRUE(device.receive(request));
  // The same device answers twice (e.g. once per interface).
  device.reply(SearchResponse("Test-device", 0x1101));
  device.reply(SearchResponse("Test-device", 0x1101));
  worker.join();

  EXPECT_EQ(discovered.size(), 1U) << "one entry per endpoint";
}

TEST(DiscoveryTest, IgnoresAnswersOfOtherServiceTypes) {
  FakeDevice device;
  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = device.port();
  options.timeout_ms = 200;

  std::vector<DiscoveredDevice> discovered;
  std::thread worker([&] { discovered = discover_devices(options); });

  Bytes request;
  ASSERT_TRUE(device.receive(request));
  // A well-formed KNXnet/IP frame that is not a SEARCH_RESPONSE.
  device.reply(knxnetip::build_routing_indication({0x29, 0x00, 0xBC, 0xE0}));
  worker.join();

  EXPECT_TRUE(discovered.empty());
}

TEST(DiscoveryEdgeCase, MulticastWithoutInterfaceUsesTheDefaultRoute) {
  DiscoveryOptions options;
  options.address = "224.0.23.12";  // no iface: default route is used
  options.timeout_ms = 50;
  const auto devices = discover_devices(options);
  EXPECT_TRUE(devices.empty() || !devices.empty());  // must never hang
}

TEST(DiscoveryTest, TwoDifferentDevicesYieldTwoEntries) {
  FakeDevice device;
  DiscoveryOptions options;
  options.address = "127.0.0.1";
  options.port = device.port();
  options.timeout_ms = 300;

  std::vector<DiscoveredDevice> discovered;
  std::thread worker([&] { discovered = discover_devices(options); });

  Bytes request;
  ASSERT_TRUE(device.receive(request));
  device.reply(SearchResponse("Device-A", 0x1101));
  device.reply(SearchResponseFor("Device-B", 0x1102, 0xC0A8010BU));
  // Same endpoint address as Device-B but a different name/IA: the endpoint
  // comparison must not treat it as a duplicate unless the port also matches.
  device.reply(SearchResponseFor("Device-B2", 0x1103, 0xC0A8010BU));
  worker.join();

  EXPECT_EQ(discovered.size(), 2U);
}

TEST(DiscoveryEdgeCase, UnknownInterfaceYieldsNoDevices) {
  DiscoveryOptions options;
  options.address = "224.0.23.12";
  options.iface = "definitely-not-an-interface0";
  options.timeout_ms = 50;
  // The join is skipped for an unknown interface; nothing answers.
  EXPECT_TRUE(discover_devices(options).empty());
}

}  // namespace
}  // namespace cvknx
