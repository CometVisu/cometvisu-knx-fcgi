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
 * @file test_knxnetip_frame_negative.cpp
 * @brief Every rejection path of the KNXnet/IP codec (coverage gate).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../src/knxnetip/cemi.h"
#include "../../src/knxnetip/knxnetip_frame.h"

namespace cvknx::knxnetip {
namespace {

using Bytes = std::vector<std::uint8_t>;

TEST(FrameNegative, ParseFrameRejectsNullAndShortBuffers) {
  EXPECT_FALSE(parse_frame(nullptr, 0).has_value());
  EXPECT_FALSE(parse_frame(static_cast<const std::uint8_t*>(nullptr), 6).has_value());
  EXPECT_FALSE(parse_frame(Bytes{0x06, 0x10, 0x03, 0x30}).has_value());
}

TEST(FrameNegative, StatusTextCoversEveryDefinedCode) {
  for (std::uint8_t status = 0x00; status <= 0x0B; ++status) {
    const std::string text = connect_status_text(status);
    EXPECT_NE(text, "unknown status") << "status " << static_cast<int>(status);
    EXPECT_FALSE(text.empty());
  }
  EXPECT_EQ(std::string{connect_status_text(0x7F)}, "unknown status");
}

TEST(FrameNegative, ParseHpaiRejectsNullAndShortBuffers) {
  EXPECT_FALSE(parse_hpai(nullptr, 8).has_value());
  const Bytes hpai = encode_hpai(Hpai{0xC0A80101U, 3671});
  for (std::size_t length = 0; length < hpai.size(); ++length) {
    EXPECT_FALSE(parse_hpai(hpai.data(), length).has_value()) << "length " << length;
  }
}

TEST(FrameNegative, ConnectResponseRejectsWrongService) {
  const auto frame = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_connect_response(frame).has_value());
}

TEST(FrameNegative, ConnectResponseRejectsBadHpai) {
  // Correct length, but the HPAI advertises IPv4/TCP (0x02).
  const Bytes frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x14, 0x01, 0x00, 0x08, 0x02,
                       0xC0, 0xA8, 0x01, 0x0A, 0x0E, 0x57, 0x04, 0x04, 0x11, 0x01};
  EXPECT_FALSE(parse_connect_response(frame).has_value());
}

TEST(FrameNegative, StatusResponseRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_status_response(routing, ServiceType::ConnectionStateResponse).has_value());

  const auto too_short = build_frame(ServiceType::ConnectionStateResponse, {0x01});
  EXPECT_FALSE(parse_status_response(too_short, ServiceType::ConnectionStateResponse).has_value());
}

TEST(FrameNegative, TunnellingRequestRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_tunnelling_request(routing).has_value());

  // Structure length byte wrong (0x03 instead of 0x04).
  const auto bad_structure = build_frame(ServiceType::TunnellingRequest, {0x03, 0x01, 0x00, 0x00});
  EXPECT_FALSE(parse_tunnelling_request(bad_structure).has_value());

  const auto too_short = build_frame(ServiceType::TunnellingRequest, {0x04, 0x01});
  EXPECT_FALSE(parse_tunnelling_request(too_short).has_value());
}

TEST(FrameNegative, TunnellingAckRejectsWrongServiceAndShortBody) {
  const auto routing = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_tunnelling_ack(routing).has_value());

  const auto bad_structure =
      build_frame(ServiceType::TunnellingAck, {0x03, 0x01, 0x00, 0x00, 0x00});
  EXPECT_FALSE(parse_tunnelling_ack(bad_structure).has_value());

  const auto too_short = build_frame(ServiceType::TunnellingAck, {0x04, 0x01});
  EXPECT_FALSE(parse_tunnelling_ack(too_short).has_value());
}

TEST(FrameNegative, TunnellingAckRejectsAFiveByteBody) {
  // The request has a reserved byte before the cEMI; the ACK does not.  A
  // device sends four bytes — a five byte body (the old, wrong layout) must be
  // rejected instead of silently reading the status from the wrong offset.
  const auto five_bytes = build_frame(ServiceType::TunnellingAck, {0x04, 0x01, 0x07, 0x00, 0x00});
  EXPECT_FALSE(parse_tunnelling_ack(five_bytes).has_value());

  // Right length, wrong structure length byte.
  const auto wrong_structure = build_frame(ServiceType::TunnellingAck, {0x03, 0x01, 0x07, 0x00});
  EXPECT_FALSE(parse_tunnelling_ack(wrong_structure).has_value());
}

TEST(FrameNegative, TunnellingAckAcceptsTheFourByteDeviceLayout) {
  const auto ack = build_frame(ServiceType::TunnellingAck, {0x04, 0x21, 0x07, 0x00});
  const auto parsed = parse_tunnelling_ack(ack);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->channel, 0x21);
  EXPECT_EQ(parsed->sequence, 0x07);
  EXPECT_EQ(parsed->status, 0x00);
}

TEST(FrameNegative, RoutingIndicationRejectsWrongServiceAndEmptyBody) {
  const auto empty = build_frame(ServiceType::RoutingIndication, {});
  EXPECT_FALSE(parse_routing_indication(empty).has_value());

  const auto wrong = build_frame(ServiceType::SearchRequest, {0x29});
  EXPECT_FALSE(parse_routing_indication(wrong).has_value());
}

TEST(FrameNegative, SearchResponseRejectsWrongServiceAndShortBody) {
  const auto wrong = build_frame(ServiceType::RoutingIndication, {0x29});
  EXPECT_FALSE(parse_search_response(wrong).has_value());

  const auto too_short = build_frame(ServiceType::SearchResponse, Bytes(8, 0x00));
  EXPECT_FALSE(parse_search_response(too_short).has_value());
}

TEST(FrameNegative, SearchResponseRejectsBadHpai) {
  Bytes body(16, 0x00);  // both "HPAIs" have length 0
  const auto frame = build_frame(ServiceType::SearchResponse, body);
  EXPECT_FALSE(parse_search_response(frame).has_value());
}

TEST(FrameNegative, SearchResponseStopsAtMalformedDib) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.push_back(0x00);  // DIB length 0 — malformed
  body.push_back(0x01);
  const auto frame = build_frame(ServiceType::SearchResponse, body);

  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->device_name.empty());
}

TEST(FrameNegative, SearchResponseSkipsForeignKeyDibsAndReadsFullName) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());

  // Supported service families DIB (type 0x02) — must be skipped.
  body.push_back(0x04);
  body.push_back(0x02);
  body.push_back(0x02);
  body.push_back(0x01);

  // Device information DIB with a name that fills all 30 bytes (no NUL).
  Bytes dib(54, 0x41);  // 'A'
  dib[0] = 54;
  dib[1] = 0x01;
  dib[4] = 0x11;  // individual address high
  dib[5] = 0x01;
  body.insert(body.end(), dib.begin(), dib.end());

  const auto frame = build_frame(ServiceType::SearchResponse, body);
  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->device_name.size(), 30U);
  EXPECT_EQ(parsed->individual_address, 0x1101);
}

TEST(FrameNegative, EmptyBodyFrameRoundTrips) {
  const auto frame = build_frame(ServiceType::DescriptionRequest, {});
  const auto parsed = parse_frame(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->body.empty());
}

TEST(FrameNegative, HpaiRejectsWrongLengthByte) {
  Bytes hpai = encode_hpai(Hpai{0xC0A80101U, 3671});
  hpai[0] = 0x07;
  EXPECT_FALSE(parse_hpai(hpai.data(), hpai.size()).has_value());
}

TEST(FrameNegative, SearchResponseRejectsBadSecondHpai) {
  Bytes body;
  const auto good = encode_hpai(Hpai{0xC0A8010AU, 3671});
  auto bad = good;
  bad[1] = 0x02;  // IPv4/TCP — not supported
  body.insert(body.end(), good.begin(), good.end());
  body.insert(body.end(), bad.begin(), bad.end());

  const auto frame = build_frame(ServiceType::SearchResponse, body);
  EXPECT_FALSE(parse_search_response(frame).has_value());
}

TEST(FrameNegative, EveryParserRejectsAnUnparsableFrame) {
  const Bytes garbage = {0x01, 0x02, 0x03};
  EXPECT_FALSE(parse_connect_response(garbage).has_value());
  EXPECT_FALSE(parse_tunnelling_request(garbage).has_value());
  EXPECT_FALSE(parse_tunnelling_ack(garbage).has_value());
  EXPECT_FALSE(parse_routing_indication(garbage).has_value());
  EXPECT_FALSE(parse_search_response(garbage).has_value());
  EXPECT_FALSE(parse_status_response(garbage, ServiceType::DisconnectResponse).has_value());
}

TEST(FrameNegative, SearchResponseRejectsDibLongerThanTheFrame) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.push_back(0xF0);  // claims 240 bytes, only 2 follow
  body.push_back(0x01);
  body.push_back(0x00);
  body.push_back(0x00);

  const auto frame = build_frame(ServiceType::SearchResponse, body);
  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->device_name.empty());
}

TEST(FrameNegative, SearchResponseSkipsShortDeviceInfoDib) {
  Bytes body;
  const auto hpai = encode_hpai(Hpai{0xC0A8010AU, 3671});
  body.insert(body.end(), hpai.begin(), hpai.end());
  body.insert(body.end(), hpai.begin(), hpai.end());
  // Device-info DIB type but only 20 bytes long: skipped, not parsed.
  Bytes dib(20, 0x00);
  dib[0] = 20;
  dib[1] = 0x01;
  body.insert(body.end(), dib.begin(), dib.end());

  const auto frame = build_frame(ServiceType::SearchResponse, body);
  const auto parsed = parse_search_response(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->device_name.empty());
  EXPECT_EQ(parsed->individual_address, 0);
}

/// The zero-copy routing payload validator must apply the same checks as
/// parse_frame() / parse_routing_indication().
TEST(KnxnetipFrameNegativeTest, RoutingPayloadRejectsMalformedFrames) {
  const Bytes valid = knxnetip::build_routing_indication(
      cemi::encode_group_indication(0x1101, 0x0A03, cemi::Bytes{0x00, 0x80, 0x42}));

  // Valid frame: the payload is a view into the buffer.
  const auto payload = knxnetip::parse_routing_payload(valid);
  ASSERT_TRUE(payload.has_value());
  EXPECT_EQ(payload->size(), valid.size() - 6);

  // Too short for the header.
  EXPECT_FALSE(knxnetip::parse_routing_payload(std::span<const std::uint8_t>{}).has_value());
  EXPECT_FALSE(knxnetip::parse_routing_payload(Bytes{0x06}).has_value());

  // Wrong header length / protocol version.
  Bytes bad_header = valid;
  bad_header[0] = 0x07;
  EXPECT_FALSE(knxnetip::parse_routing_payload(bad_header).has_value());
  bad_header = valid;
  bad_header[1] = 0x20;
  EXPECT_FALSE(knxnetip::parse_routing_payload(bad_header).has_value());

  // Not a ROUTING_INDICATION.
  EXPECT_FALSE(knxnetip::parse_routing_payload(knxnetip::build_search_request(knxnetip::Hpai{}))
                   .has_value());

  // Length field does not match the datagram.
  Bytes bad_length = valid;
  bad_length[4] = 0x00;
  bad_length[5] = 0x08;
  EXPECT_FALSE(knxnetip::parse_routing_payload(bad_length).has_value());

  // A bare header carries no payload.
  const Bytes header_only = {0x06, 0x10, 0x05, 0x30, 0x00, 0x06};
  EXPECT_FALSE(knxnetip::parse_routing_payload(header_only).has_value());
}

}  // namespace
}  // namespace cvknx::knxnetip
