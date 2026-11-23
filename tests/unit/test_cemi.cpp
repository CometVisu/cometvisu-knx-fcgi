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
 * @file test_cemi.cpp
 * @brief Hex vectors are the contract for the cEMI codec.
 *
 * The decoder must handle every cEMI message type a KNXnet/IP interface can
 * emit: it has to *drop* what is irrelevant (confirmations, requests, bus
 * monitor, raw frames, property access, individual addressing) and parse a
 * group `L_Data.ind` correctly, including a non-empty additional info block
 * and both the standard and the extended frame format.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/knxnetip/cemi.h"

namespace cvknx::cemi {
namespace {

using Bytes = std::vector<std::uint8_t>;

// ===========================================================================
// Encoding
// ===========================================================================

TEST(CemiTest, EncodeGroupWriteOneByte) {
  const auto frame = encode_group_request(0x1101, 0x0A03, Bytes{0x00, 0x80, 0x42});
  const Bytes expected = {0x11, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_EQ(frame, expected);
}

TEST(CemiTest, EncodeGroupWriteTwoBytes) {
  const auto frame = encode_group_request(0x0000, 0x0A03, Bytes{0x00, 0x80, 0x0C, 0x6F});
  ASSERT_EQ(frame.size(), 13U);
  EXPECT_EQ(frame[8], 0x03);  // length byte = APDU octets - 1
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

TEST(CemiTest, EncodeGroupValueRead) {
  const auto frame = encode_group_request(0x1101, 0x0A03, Bytes{0x00, 0x00});
  ASSERT_EQ(frame.size(), 11U);
  EXPECT_EQ(frame[8], 0x01);
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x00}));
}

TEST(CemiTest, EncodeEmptyApduRejected) {
  EXPECT_TRUE(encode_group_request(0x1101, 0x0A03, Bytes{}).empty());
  EXPECT_TRUE(encode_group_indication(0x1101, 0x0A03, Bytes{}).empty());
}

TEST(CemiTest, EncodeGroupIndicationOneByte) {
  // Routing participants send indications: the message code is 0x29.
  const auto frame = encode_group_indication(0xFFFA, 0x0A03, Bytes{0x00, 0x80, 0x42});
  const Bytes expected = {0x29, 0x00, 0xBC, 0xE0, 0xFF, 0xFA, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_EQ(frame, expected);
}

TEST(CemiTest, EncodeGroupIndicationTwoBytes) {
  const auto frame = encode_group_indication(0x1101, 0x0A03, Bytes{0x00, 0x80, 0x0C, 0x6F});
  ASSERT_EQ(frame.size(), 13U);
  EXPECT_EQ(frame[0], kMessageCodeLDataInd);
  EXPECT_EQ(frame[8], 0x03);  // length byte = APDU octets - 1
  EXPECT_EQ(Bytes(frame.begin() + 9, frame.end()), (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

TEST(CemiTest, IndicationRoundTripsThroughTheParser) {
  const auto frame = encode_group_indication(0x1101, 0x0A04, Bytes{0x00, 0x80, 0x01});
  const auto parsed = parse_ldata_ind(frame);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->source, 0x1101);
  EXPECT_EQ(parsed->destination, 0x0A04);
  EXPECT_EQ(parsed->apdu, (Bytes{0x00, 0x80, 0x01}));
}

// ===========================================================================
// Decoding: the interesting case
// ===========================================================================

TEST(CemiTest, ParseLDataIndGroupWrite) {
  const Bytes raw = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->source, 0x1101);
  EXPECT_EQ(frame->destination, 0x0A03);
  EXPECT_EQ(frame->apdu, (Bytes{0x00, 0x80, 0x42}));
}

/// A frame carrying additional info (legal, e.g. from a coupler) must be
/// parsed with every field taken *after* the additional info block.
TEST(CemiTest, ParseLDataIndSkipsAdditionalInfo) {
  // additional_info_len = 3; the three octets 0xAA 0xBB 0xCC must be skipped.
  const Bytes raw = {0x29, 0x03, 0xAA, 0xBB, 0xCC, 0xBC, 0xE0, 0x11,
                     0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->source, 0x1101) << "control fields were read at the wrong offset";
  EXPECT_EQ(frame->destination, 0x0A03);
  EXPECT_EQ(frame->apdu, (Bytes{0x00, 0x80, 0x42}));
}

/// The standard frame format (control 1 bit 7 = 1) and the extended frame
/// format (bit 7 = 0) share the cEMI layout; both must parse.
TEST(CemiTest, ParseLDataIndAcceptsExtendedFrameFormat) {
  // Extended frame: 0x3C = 0b00111100 (bit 7 clear), hop count 6.
  const Bytes raw = {0x29, 0x00, 0x3C, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->destination, 0x0A03);
}

// ===========================================================================
// Decoding: everything that must be dropped, early and unambiguously
// ===========================================================================

TEST(CemiTest, ParseLDataIndIgnoresIndividualDestination) {
  // Control field 2 without the group bit: programming / management traffic.
  const Bytes raw = {0x29, 0x00, 0xBC, 0x60, 0x11, 0x01, 0x11, 0x02, 0x01, 0x00, 0x81};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

TEST(CemiTest, ParseLDataIndDropsConfirmations) {
  // 0x2E = L_Data.con: our own write must never reach the cache.
  const Bytes raw = {0x2E, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

/// Every other cEMI message type is irrelevant for this backend.  None of
/// them may be mistaken for a group value — even though several carry the
/// same byte pattern after their (different) header.
TEST(CemiTest, ParseDropsEveryOtherMessageCode) {
  const Bytes payload = {0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  for (const std::uint8_t code :
       {kMessageCodeLRawReq, kMessageCodeLDataReq, kMessageCodeLPollDataReq, kMessageCodeLBusyReq,
        kMessageCodeLBusyInd, kMessageCodeLPollDataCon, kMessageCodeLBusmonInd, kMessageCodeLRawInd,
        kMessageCodeLRawCon, kMessageCodeMResetInd, kMessageCodeMResetReq,
        kMessageCodeMPropWriteCon, kMessageCodeMPropWriteReq, kMessageCodeMPropInfoInd,
        kMessageCodeMFuncPropCommand, kMessageCodeMFuncPropStateRead,
        kMessageCodeMFuncPropCommandCon, kMessageCodeMPropReadCon, kMessageCodeMPropReadReq}) {
    Bytes raw;
    raw.push_back(code);
    raw.insert(raw.end(), payload.begin(), payload.end());
    EXPECT_FALSE(parse_ldata_ind(raw).has_value())
        << "message code 0x" << std::hex << static_cast<int>(code) << " was accepted";
  }
}

TEST(CemiTest, ParseTruncatedFrameRejected) {
  const Bytes full = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42};
  for (std::size_t len = 0; len < full.size(); ++len) {
    const Bytes prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
    EXPECT_FALSE(parse_ldata_ind(prefix).has_value()) << "prefix length " << len;
  }
}

TEST(CemiTest, ParsePaddedFrameRejected) {
  Bytes raw = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x80, 0x42, 0x00};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

/// An additional info length that exceeds the buffer must be rejected without
/// reading past the end.
TEST(CemiTest, ParseAdditionalInfoBeyondBufferRejected) {
  const Bytes raw = {0x29, 0xF0, 0xAA, 0xBB, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x02, 0x00};
  EXPECT_FALSE(parse_ldata_ind(raw).has_value());
}

/// A frame with a single APDU octet (length byte 0) is legal.
TEST(CemiTest, ParseSingleOctetApdu) {
  const Bytes raw = {0x29, 0x00, 0xBC, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x00, 0x00};
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  ASSERT_EQ(frame->apdu.size(), 1U);
  EXPECT_EQ(frame->apdu[0], 0x00);
}

TEST(CemiTest, ParseLongApdu) {
  // Extended frames can carry up to 254 APDU octets; the length octet drives
  // the parse, so a long value is accepted as-is.
  Bytes raw = {0x29, 0x00, 0x3C, 0xE0, 0x11, 0x01, 0x0A, 0x03, 0x0F};
  for (std::uint8_t i = 0; i < 16; ++i) {
    raw.push_back(i);
  }
  const auto frame = parse_ldata_ind(raw);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->apdu.size(), 16U);
}

TEST(CemiTest, ParseEmptyBufferAndNullptr) {
  EXPECT_FALSE(parse_ldata_ind(Bytes{}).has_value());
  EXPECT_FALSE(parse_ldata_ind(std::span<const std::uint8_t>{}).has_value());
}

/// The cEMI length octet is (APDU octets - 1): at most 256 octets fit.  A
/// longer APDU must be rejected, not encoded with a truncated length.
TEST(CemiTest, EncodeRejectsAnApduThatDoesNotFitTheLengthOctet) {
  const Bytes apdu(257, 0x00);
  EXPECT_TRUE(encode_group_request(0x1102, 0x0A03, apdu).empty());
  EXPECT_TRUE(encode_group_indication(0x1102, 0x0A03, apdu).empty());
}

}  // namespace
}  // namespace cvknx::cemi
