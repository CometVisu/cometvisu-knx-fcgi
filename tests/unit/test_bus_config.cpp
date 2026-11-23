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
 * @file test_bus_config.cpp
 * @brief KNX_TRANSPORT is mandatory and every value is validated.
 */

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "../../src/bus/bus_config.h"

namespace cvknx {
namespace {

/// Parse using a fixed environment map (no process-global environment).
std::optional<BusConfig> parse_env(const std::map<std::string, std::string>& env,
                                   std::string& error) {
  const BusConfig::EnvGetter get = [&env](const char* name) -> std::string {
    const auto it = env.find(name);
    return it != env.end() ? it->second : std::string{};
  };
  return BusConfig::parse(get, error);
}

TEST(BusConfigTest, TransportIsRequired) {
  std::string error;
  const auto cfg = parse_env({}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_TRANSPORT is required"), std::string::npos) << error;
}

TEST(BusConfigTest, UnknownTransportIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "eibd"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("unknown KNX_TRANSPORT 'eibd'"), std::string::npos) << error;
}

TEST(BusConfigTest, KnxdKeepsCurrentDefaults) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::Knxd);
  EXPECT_EQ(cfg->knxd_socket, "/run/knx");
  EXPECT_EQ(cfg->ip_port, 3671);
}

TEST(BusConfigTest, KnxdSocketCanBeOverridden) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNXD_SOCKET", "/tmp/knxd"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->knxd_socket, "/tmp/knxd");
}

TEST(BusConfigTest, IpRoutingNeedsNoHost) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-routing"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::IpRouting);
  EXPECT_EQ(cfg->ip_multicast, "224.0.23.12");
  EXPECT_EQ(cfg->ip_individual_addr, 0xFFFA);  // 15.15.250
}

TEST(BusConfigTest, IpRoutingParsesTheIndividualAddress) {
  std::string error;
  const auto cfg =
      parse_env({{"KNX_TRANSPORT", "ip-routing"}, {"KNX_IP_INDIVIDUAL_ADDR", "1.1.240"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->ip_individual_addr, 0x11F0);
}

TEST(BusConfigTest, InvalidIndividualAddressIsRejected) {
  for (const char* value :
       {"1.1", "1.1.0", "16.1.1", "1.16.1", "1.1.256", "1.1.1x", "0.0.0", "-1.1.1"}) {
    std::string error;
    const auto cfg =
        parse_env({{"KNX_TRANSPORT", "ip-routing"}, {"KNX_IP_INDIVIDUAL_ADDR", value}}, error);
    EXPECT_FALSE(cfg.has_value()) << "accepted '" << value << "'";
    EXPECT_NE(error.find("KNX_IP_INDIVIDUAL_ADDR"), std::string::npos) << error;
  }
}

TEST(BusConfigTest, IndividualAddressAcceptsBoundaryValues) {
  for (const auto& [text, expected] : std::vector<std::pair<const char*, std::uint16_t>>{
           {"0.0.1", 0x0001}, {"15.15.255", 0xFFFF}, {"1.1.1", 0x1101}}) {
    std::string error;
    const auto cfg =
        parse_env({{"KNX_TRANSPORT", "ip-routing"}, {"KNX_IP_INDIVIDUAL_ADDR", text}}, error);
    ASSERT_TRUE(cfg.has_value()) << text << ": " << error;
    EXPECT_EQ(cfg->ip_individual_addr, expected) << text;
  }
}

TEST(BusConfigTest, IpRoutingParsesMulticastAndInterface) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-routing"},
                              {"KNX_IP_MULTICAST", "239.0.0.1"},
                              {"KNX_IP_IFACE", "eth0"},
                              {"KNX_IP_PORT", "3672"}},
                             error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->ip_multicast, "239.0.0.1");
  EXPECT_EQ(cfg->ip_iface, "eth0");
  EXPECT_EQ(cfg->ip_port, 3672);
}

TEST(BusConfigTest, IpTunnelRequiresHost) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-tunnel"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_HOST is required"), std::string::npos) << error;
}

TEST(BusConfigTest, IpTunnelParsesAllOptions) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-tunnel"},
                              {"KNX_IP_HOST", "192.168.1.10"},
                              {"KNX_IP_LOCAL_ADDR", "192.168.1.20"},
                              {"KNX_IP_NAT", "1"},
                              {"KNX_IP_HEARTBEAT_SEC", "30"},
                              {"KNX_IP_ACK_TIMEOUT_MS", "500"},
                              {"KNX_IP_RETRIES", "5"}},
                             error);
  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::IpTunnel);
  EXPECT_EQ(cfg->ip_host, "192.168.1.10");
  EXPECT_EQ(cfg->ip_local_addr, "192.168.1.20");
  EXPECT_TRUE(cfg->ip_nat);
  EXPECT_EQ(cfg->heartbeat_sec, 30);
  EXPECT_EQ(cfg->ack_timeout_ms, 500);
  EXPECT_EQ(cfg->retries, 5);
}

TEST(BusConfigTest, InvalidPortIsRejectedNotEmptyDefault) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "70000"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_PORT"), std::string::npos) << error;
}

TEST(BusConfigTest, GarbagePortIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "36x1"}}, error);
  ASSERT_FALSE(cfg.has_value());
}

TEST(BusConfigTest, HeartbeatBelowTenIsRejected) {
  std::string error;
  const auto cfg = parse_env(
      {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_HEARTBEAT_SEC", "5"}},
      error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_HEARTBEAT_SEC"), std::string::npos) << error;
}

TEST(BusConfigTest, ZeroRetriesIsRejected) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_RETRIES", "0"}}, error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_RETRIES"), std::string::npos) << error;
}

TEST(BusConfigTest, InvalidNatFlagIsRejected) {
  std::string error;
  const auto cfg = parse_env(
      {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_NAT", "maybe"}},
      error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("KNX_IP_NAT"), std::string::npos) << error;
}

TEST(BusConfigTest, OutageKeyIsTransportDependent) {
  std::string error;
  const auto knxd = parse_env({{"KNX_TRANSPORT", "knxd"}}, error);
  ASSERT_TRUE(knxd.has_value());
  EXPECT_EQ(knxd->outage_key(), "knxd");

  const auto routing = parse_env({{"KNX_TRANSPORT", "ip-routing"}}, error);
  ASSERT_TRUE(routing.has_value());
  EXPECT_EQ(routing->outage_key(), "bus");

  const auto tunnel =
      parse_env({{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}}, error);
  ASSERT_TRUE(tunnel.has_value());
  EXPECT_EQ(tunnel->outage_key(), "bus");
}

TEST(BusConfigTest, DescribeNamesTheTransport) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNXD_SOCKET", "/tmp/knxd"}}, error);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_NE(cfg->describe().find("knxd"), std::string::npos);
  EXPECT_NE(cfg->describe().find("/tmp/knxd"), std::string::npos);
}

TEST(BusConfigTest, TransportNamesRoundTrip) {
  for (const char* name : {"knxd", "ip-tunnel", "ip-routing"}) {
    const auto parsed = bus_transport_from_string(name);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_EQ(std::string{bus_transport_to_string(*parsed)}, name);
  }
  EXPECT_FALSE(bus_transport_from_string("IP-TUNNEL").has_value());
  EXPECT_FALSE(bus_transport_from_string("").has_value());
}

TEST(BusConfigTest, DescribeCoversEveryTransport) {
  std::string error;

  const auto tunnel = parse_env({{"KNX_TRANSPORT", "ip-tunnel"},
                                 {"KNX_IP_HOST", "10.1.2.3"},
                                 {"KNX_IP_PORT", "4000"},
                                 {"KNX_IP_LOCAL_ADDR", "10.1.2.4"},
                                 {"KNX_IP_NAT", "1"}},
                                error);
  ASSERT_TRUE(tunnel.has_value()) << error;
  const std::string tunnel_text = tunnel->describe();
  EXPECT_NE(tunnel_text.find("ip-tunnel"), std::string::npos);
  EXPECT_NE(tunnel_text.find("10.1.2.3:4000"), std::string::npos);
  EXPECT_NE(tunnel_text.find("local=10.1.2.4"), std::string::npos);
  EXPECT_NE(tunnel_text.find("nat=1"), std::string::npos);

  const auto routing = parse_env({{"KNX_TRANSPORT", "ip-routing"},
                                  {"KNX_IP_MULTICAST", "239.0.0.9"},
                                  {"KNX_IP_IFACE", "eth0"}},
                                 error);
  ASSERT_TRUE(routing.has_value()) << error;
  const std::string routing_text = routing->describe();
  EXPECT_NE(routing_text.find("239.0.0.9:3671"), std::string::npos);
  EXPECT_NE(routing_text.find("iface=eth0"), std::string::npos);

  // Without an interface the "iface=" part is omitted.
  const auto routing_default = parse_env({{"KNX_TRANSPORT", "ip-routing"}}, error);
  ASSERT_TRUE(routing_default.has_value());
  EXPECT_EQ(routing_default->describe().find("iface="), std::string::npos);
}

TEST(BusConfigTest, BooleanNATAcceptsEverySpelling) {
  for (const char* enabled : {"1", "true", "yes", "on"}) {
    std::string error;
    const auto cfg = parse_env(
        {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_NAT", enabled}},
        error);
    ASSERT_TRUE(cfg.has_value()) << enabled << ": " << error;
    EXPECT_TRUE(cfg->ip_nat) << enabled;
  }
  for (const char* disabled : {"0", "false", "no", "off"}) {
    std::string error;
    const auto cfg = parse_env(
        {{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}, {"KNX_IP_NAT", disabled}},
        error);
    ASSERT_TRUE(cfg.has_value()) << disabled << ": " << error;
    EXPECT_FALSE(cfg->ip_nat) << disabled;
  }
}

TEST(BusConfigTest, IntegerUpperBoundsAreEnforced) {
  std::string error;
  EXPECT_FALSE(parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "65536"}}, error).has_value());
  EXPECT_FALSE(parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_RETRIES", "11"}}, error).has_value());
  EXPECT_FALSE(
      parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_ACK_TIMEOUT_MS", "5"}}, error).has_value());
  EXPECT_FALSE(parse_env({{"KNX_TRANSPORT", "ip-tunnel"},
                          {"KNX_IP_HOST", "10.0.0.1"},
                          {"KNX_IP_HEARTBEAT_SEC", "999999"}},
                         error)
                   .has_value());
}

TEST(BusConfigTest, FromEnvReadsTheProcessEnvironment) {
  ::setenv("KNX_TRANSPORT", "ip-routing", 1);
  ::setenv("KNX_IP_PORT", "4001", 1);
  std::string error;
  const auto cfg = BusConfig::from_env(error);
  ::unsetenv("KNX_TRANSPORT");
  ::unsetenv("KNX_IP_PORT");

  ASSERT_TRUE(cfg.has_value()) << error;
  EXPECT_EQ(cfg->transport, BusTransport::IpRouting);
  EXPECT_EQ(cfg->ip_port, 4001);
}

TEST(BusConfigTest, TunnelWithoutLocalAddressOrNatOmitsBothInDescribe) {
  std::string error;
  const auto cfg = parse_env({{"KNX_TRANSPORT", "ip-tunnel"}, {"KNX_IP_HOST", "10.0.0.1"}}, error);
  ASSERT_TRUE(cfg.has_value()) << error;
  const std::string text = cfg->describe();
  EXPECT_EQ(text.find("local="), std::string::npos);
  EXPECT_EQ(text.find("nat="), std::string::npos);
}

TEST(BusConfigTest, FromEnvWithoutTransportReportsTheRequirement) {
  ::unsetenv("KNX_TRANSPORT");
  std::string error;
  const auto cfg = BusConfig::from_env(error);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(error.find("required"), std::string::npos);
}

TEST(BusConfigTest, OverflowingNumberIsRejected) {
  // strtol() sets ERANGE and returns LONG_MAX; the value must be rejected,
  // never silently clamped to the maximum.
  std::string error;
  EXPECT_FALSE(
      parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "99999999999999999999"}}, error)
          .has_value());
  EXPECT_NE(error.find("KNX_IP_PORT"), std::string::npos) << error;
}

TEST(BusConfigTest, NonNumericValueIsRejected) {
  std::string error;
  EXPECT_FALSE(parse_env({{"KNX_TRANSPORT", "knxd"}, {"KNX_IP_PORT", "abc"}}, error).has_value());
  EXPECT_NE(error.find("KNX_IP_PORT"), std::string::npos) << error;
}

}  // namespace
}  // namespace cvknx
