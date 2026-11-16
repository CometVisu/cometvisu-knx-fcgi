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
 * @file test_knxnetip_frame.cpp
 * @brief Framing contract: complete byte vectors, not individual offsets.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/knxnetip/knxnetip_frame.h"

namespace cvknx::knxnetip {
namespace {

using Bytes = std::vector<std::uint8_t>;

const Hpai kControl{0xC0A80114U, 3671};  // 192.168.1.20:3671
const Hpai kAnyHost{0U, 0U};             // 0.0.0.0:0

TEST(KnxnetIpFrameTest, HeaderRoundTripForEveryService) {
  const Bytes body = {0x01, 0x02, 0x03};
  for (const unsigned raw : {0x0201U, 0x0202U, 0x0203U, 0x0204U, 0x0205U, 0x0206U, 0x0207U, 0x0208U,
                             0x0209U, 0x020AU, 0x0420U, 0x0421U, 0x0530U, 0x0531U, 0x0532U}) {
    const auto service = static_cast<ServiceType>(static_cast<std::uint16_t>(raw));
    const auto frame = build_frame(service, body);
    ASSERT_EQ(frame.size(), 6U + body.size());
    EXPECT_EQ(frame[0], 0x06);
    EXPECT_EQ(frame[1], 0x10);
    const auto parsed = parse_frame(frame);
    ASSERT_TRUE(parsed.has_value()) << std::hex << static_cast<unsigned>(raw);
    EXPECT_EQ(static_cast<unsigned>(parsed->service), raw);
    EXPECT_EQ(parsed->body, body);
  }
}

TEST(KnxnetIpFrameTest, RoutingServicesUseTheDocumentedCodes) {
  // KNXnet/IP 03_08_01 5.*: tunnelling is 0x042x, routing is 0x053x.  Real
  // devices drop a frame with any other service code, so these must never
  // drift again.
  EXPECT_EQ(static_cast<unsigned>(ServiceType::TunnellingRequest), 0x0420U);
  EXPECT_EQ(static_cast<unsigned>(ServiceType::TunnellingAck), 0x0421U);
  EXPECT_EQ(static_cast<unsigned>(ServiceType::RoutingIndication), 0x0530U);
  EXPECT_EQ(static_cast<unsigned>(ServiceType::RoutingLostMessage), 0x0531U);
  EXPECT_EQ(static_cast<unsigned>(ServiceType::RoutingBusy), 0x0532U);
}

TEST(KnxnetIpFrameTest, RejectsWrongHeaderLength) {
  auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  frame[0] = 0x05;
  EXPECT_FALSE(parse_frame(frame).has_value());
}

TEST(KnxnetIpFrameTest, RejectsUnknownVersion) {
  auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  frame[1] = 0x20;
  EXPECT_FALSE(parse_frame(frame).has_value());
}

TEST(KnxnetIpFrameTest, RejectsLengthMismatch) {
  auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  frame[5] = static_cast<std::uint8_t>(frame[5] + 1);
  EXPECT_FALSE(parse_frame(frame).has_value());
}

TEST(KnxnetIpFrameTest, IgnoresUnknownServiceType) {
  auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  frame[2] = 0x7F;
  frame[3] = 0x00;
  EXPECT_FALSE(parse_frame(frame).has_value());
}

TEST(KnxnetIpFrameTest, HpaiEncodeDecode) {
  const auto encoded = encode_hpai(kControl);
  const Bytes expected = {0x08, 0x01, 0xC0, 0xA8, 0x01, 0x14, 0x0E, 0x57};
  EXPECT_EQ(encoded, expected);

  const auto parsed = parse_hpai(encoded.data(), encoded.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->address, kControl.address);
  EXPECT_EQ(parsed->port, kControl.port);
}

TEST(KnxnetIpFrameTest, HpaiAllowsUnspecifiedAddress) {
  const auto encoded = encode_hpai(kAnyHost);
  const Bytes expected = {0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(encoded, expected);
  EXPECT_TRUE(parse_hpai(encoded.data(), encoded.size()).has_value());
}

TEST(KnxnetIpFrameTest, HpaiRejectsWrongProtocol) {
  auto encoded = encode_hpai(kControl);
  encoded[1] = 0x02;
  EXPECT_FALSE(parse_hpai(encoded.data(), encoded.size()).has_value());
  EXPECT_FALSE(parse_hpai(encoded.data(), 4).has_value());
}

TEST(KnxnetIpFrameTest, IPv4Helpers) {
  const auto address = ipv4_from_string("192.168.1.20");
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ(*address, 0xC0A80114U);
  EXPECT_EQ(ipv4_to_string(*address), "192.168.1.20");
  EXPECT_FALSE(ipv4_from_string("not-an-ip").has_value());
  EXPECT_FALSE(ipv4_from_string("").has_value());
}

TEST(KnxnetIpFrameTest, ConnectRequestBytes) {
  const auto frame = build_connect_request(kControl, kAnyHost);
  const Bytes expected = {0x06, 0x10, 0x02, 0x05, 0x00, 0x1A,              // header, 26 bytes
                          0x08, 0x01, 0xC0, 0xA8, 0x01, 0x14, 0x0E, 0x57,  // HPAI control
                          0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // HPAI data
                          0x04,                                            // CRI length
                          0x04, 0x02, 0x00};  // tunnel, link layer, reserved
  EXPECT_EQ(frame, expected);
}

TEST(KnxnetIpFrameTest, ConnectRequestCriLengthMatchesTheBody) {
  // A device validates CRI[0] == body.size() - 16 and silently drops the
  // request otherwise (knxd: parseEIBnet_ConnectRequest).
  const auto frame = build_connect_request(kControl, kAnyHost);
  ASSERT_GE(frame.size(), 26U);
  const std::uint8_t cri_length = frame[22];
  EXPECT_EQ(cri_length, frame.size() - 6U - 16U) << "CRI[0] == body size - 16";
  EXPECT_EQ(cri_length, kCriLength);
  EXPECT_EQ(frame[23], kConnectionTypeTunnel);
  EXPECT_EQ(frame[24], kKnxLayerLink);
}

TEST(KnxnetIpFrameTest, ParseConnectResponseOk) {
  const Bytes frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x14, 0x01, 0x00, 0x08, 0x01,
                       0xC0, 0xA8, 0x01, 0x0A, 0x0E, 0x57, 0x04, 0x04, 0x11, 0x01};
  const auto response = parse_connect_response(frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->channel, 0x01);
  EXPECT_EQ(response->status, 0x00);
  EXPECT_EQ(response->endpoint.address, 0xC0A8010AU);
  EXPECT_EQ(response->endpoint.port, 3671);
  EXPECT_EQ(response->crd_address, 0x1101);
}

TEST(KnxnetIpFrameTest, ParseConnectResponseErrorNoMoreConnections) {
  Bytes frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x14, 0x00, 0x07, 0x08, 0x01,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00};
  const auto response = parse_connect_response(frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->status, static_cast<std::uint8_t>(ConnectStatus::NoMoreConnections));
  EXPECT_NE(std::string{connect_status_text(response->status)}.find("tunnel limit"),
            std::string::npos);
}

TEST(KnxnetIpFrameTest, ConnectResponseTruncatedRejected) {
  const Bytes frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x10, 0x01, 0x00,
                       0x08, 0x01, 0xC0, 0xA8, 0x01, 0x0A, 0x0E, 0x57};
  EXPECT_FALSE(parse_connect_response(frame).has_value());
}

TEST(KnxnetIpFrameTest, ConnectionStateRequestBytes) {
  const auto frame = build_connectionstate_request(0x05, kControl);
  ASSERT_EQ(frame.size(), 16U);
  const Bytes expected = {0x06, 0x10, 0x02, 0x07, 0x00, 0x10, 0x05, 0x00,
                          0x08, 0x01, 0xC0, 0xA8, 0x01, 0x14, 0x0E, 0x57};
  EXPECT_EQ(frame, expected);
}

TEST(KnxnetIpFrameTest, ParseConnectionStateResponse) {
  const Bytes frame = {0x06, 0x10, 0x02, 0x08, 0x00, 0x08, 0x05, 0x04};
  const auto response = parse_status_response(frame, ServiceType::ConnectionStateResponse);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->channel, 0x05);
  EXPECT_EQ(response->status, 0x04);  // unknown channel id
  // Wrong service type is rejected.
  EXPECT_FALSE(parse_status_response(frame, ServiceType::DisconnectResponse).has_value());
}

TEST(KnxnetIpFrameTest, DisconnectRequestBytes) {
  const auto frame = build_disconnect_request(0x05, kControl);
  ASSERT_EQ(frame.size(), 16U);
  EXPECT_EQ(frame[3], 0x09);
}

TEST(KnxnetIpFrameTest, TunnellingRequestBytes) {
  const Bytes cemi = {0x11, 0x00, 0xBC, 0xE0, 0x00, 0x00, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  const auto frame = build_tunnelling_request(0x01, 0x07, cemi);
  ASSERT_EQ(frame.size(), 10U + cemi.size());
  EXPECT_EQ(frame[2], 0x04);  // service type 0x0420 (tunnelling)
  EXPECT_EQ(frame[3], 0x20);
  EXPECT_EQ(frame[6], 0x04);  // structure length
  EXPECT_EQ(frame[7], 0x01);  // channel
  EXPECT_EQ(frame[8], 0x07);  // sequence
  EXPECT_EQ(Bytes(frame.begin() + 10, frame.end()), cemi);

  const auto parsed = parse_tunnelling_request(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->channel, 0x01);
  EXPECT_EQ(parsed->sequence, 0x07);
  EXPECT_EQ(parsed->cemi, cemi);
}

TEST(KnxnetIpFrameTest, TunnellingAckBytes) {
  const auto frame = build_tunnelling_ack(0x01, 0x07);
  ASSERT_EQ(frame.size(), 10U);
  const Bytes expected = {0x06, 0x10, 0x04, 0x21, 0x00, 0x0A, 0x04, 0x01, 0x07, 0x00};
  EXPECT_EQ(frame, expected);

  const auto parsed = parse_tunnelling_ack(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->channel, 0x01);
  EXPECT_EQ(parsed->sequence, 0x07);
  EXPECT_EQ(parsed->status, 0x00);
}

TEST(KnxnetIpFrameTest, ParseRoutingIndication) {
  const Bytes cemi = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x01, 0x00, 0x81};
  const auto frame = build_routing_indication(cemi);
  const auto parsed = parse_routing_indication(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, cemi);
}

TEST(KnxnetIpFrameTest, SearchRequestBytes) {
  const auto frame = build_search_request(kAnyHost);
  ASSERT_EQ(frame.size(), 14U);
  EXPECT_EQ(frame[3], 0x01);  // SearchRequest 0x0201
}

TEST(KnxnetIpFrameTest, ParseSearchResponseHardwareDib) {
  // Header (6) + HPAI control (8) + HPAI endpoint (8) + device info DIB (54).
  Bytes frame = {
      0x06, 0x10, 0x02, 0x02, 0x00, 0x00,  // length fixed below
      0x08, 0x01, 0xC0, 0xA8, 0x01, 0x64, 0x0E, 0x57, 0x08, 0x01, 0xC0, 0xA8, 0x01, 0x0A, 0x0E,
      0x57, 0x36, 0x01,                    // DIB: length 54, type device info
      0x02, 0x00,                          // medium type, status
      0x11, 0x01,                          // individual address 1.1.1
      0x00, 0x00,                          // project installation id
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05,  // serial
      0xEF, 0x00, 0x17, 0x0C,              // routing multicast 239.0.23.12
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05,  // MAC
      0x54, 0x65, 0x73, 0x74, 0x2D, 0x64, 0x65, 0x76, 0x2D, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  frame[4] = static_cast<std::uint8_t>((frame.size() >> 8) & 0xFF);
  frame[5] = static_cast<std::uint8_t>(frame.size() & 0xFF);

  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->device_name, "Test-dev-1");
  EXPECT_EQ(parsed->individual_address, 0x1101);
  EXPECT_EQ(parsed->multicast_address, 0xEF00170CU);
  EXPECT_EQ(parsed->endpoint.address, 0xC0A8010AU);
}

TEST(KnxnetIpFrameTest, SearchResponseWithoutDibIsStillUsable) {
  Bytes frame = {0x06, 0x10, 0x02, 0x02, 0x00, 0x16, 0x08, 0x01, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x08, 0x01, 0xC0, 0xA8, 0x01, 0x0A, 0x0E, 0x57};
  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->device_name.empty());
  EXPECT_EQ(parsed->individual_address, 0);
}

TEST(KnxnetIpFrameTest, BuildFrameRejectsABodyThatDoesNotFitTheLengthField) {
  // The total length field is 16 bits.  Truncating it would emit a frame that
  // describes different contents than it carries.
  const std::vector<std::uint8_t> body(70000, 0x00);
  EXPECT_TRUE(build_frame(ServiceType::RoutingIndication, body).empty());
}

}  // namespace
}  // namespace cvknx::knxnetip
