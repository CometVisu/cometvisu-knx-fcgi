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

#include <gtest/gtest.h>

#include "handlers/write_handler.h"
#include "mock_bus_link.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"
#include "util/query_string.h"

using namespace cvknx;

class WriteHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(cache_.create());
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  MockBusLink knxd_;
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  SessionStore sessions_;
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  SharedGroupCache cache_;
};

TEST_F(WriteHandlerTest, WriteSingleAddress) {
  WriteHandler handler(knxd_, sessions_);

  // v must include the APCI byte (0x80 = A_GroupValue_Write).
  // This matches the reference eibwrite-cgi.c convention.
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=8042");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3

  // APDU: [0x00] + hex-decoded value = [0x00, 0x80, 0x42]
  ASSERT_EQ(sent[0].apdu.size(), 3);
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x80);  // APCI byte
  EXPECT_EQ(sent[0].apdu[2], 0x42);  // value byte
}

TEST_F(WriteHandlerTest, WriteMultiByteValue) {
  WriteHandler handler(knxd_, sessions_);

  // v must include the APCI byte (0x80 = A_GroupValue_Write).
  // This matches the reference eibwrite-cgi.c convention.
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=800c6f");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  ASSERT_EQ(sent[0].apdu.size(), 4);
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x80);  // Write APCI
  EXPECT_EQ(sent[0].apdu[2], 0x0C);
  EXPECT_EQ(sent[0].apdu[3], 0x6F);
}

TEST_F(WriteHandlerTest, WriteMultipleAddresses) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("s=0&a=KNX:1/2/3&a=KNX:4/5/6&v=8042");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 2);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3
  EXPECT_EQ(sent[1].group_addr, 0x2506);  // 4/5/6
}

// A truncated address list would silently write to only some of the requested
// addresses — that must never happen, so report the limit violation instead.
TEST_F(WriteHandlerTest, TooManyAddressesReturns400) {
  std::string q;
  for (size_t i = 0; i < QueryString::kMaxValuesPerKey + 10; ++i) {
    q += (i == 0 ? "s=0&a=KNX:1/2/3" : "&a=KNX:1/2/3");
  }
  q += "&v=8042";

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle(q);

  EXPECT_EQ(result.http_status, 400);
  EXPECT_NE(result.body.find("\"error\":\"too many addresses\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, MissingAddress) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&v=8042");
  // Missing address: nothing to write, returns 200 (no-op)
  EXPECT_EQ(result.http_status, 200);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

// The protocol requires `s` on every request: a write from a client that never
// logged in must not toggle the bus.
TEST_F(WriteHandlerTest, MissingSessionReturns401) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=8042");
  EXPECT_EQ(result.http_status, 401);
  EXPECT_NE(result.body.find("\"error\":\"missing session\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, MissingValue) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&a=KNX:1/2/3");
  // Missing value: nothing to write, returns 200 (no-op)
  EXPECT_EQ(result.http_status, 200);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, InvalidHexValue) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=ZZ");
  // Invalid hex is a client error — answering 200 would pretend success.
  EXPECT_EQ(result.http_status, 400);
  EXPECT_NE(result.body.find("\"error\":\"invalid value\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, InvalidAddress) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&a=invalid&v=8042");
  EXPECT_EQ(result.http_status, 404);
  EXPECT_NE(result.body.find("\"error\":\"invalid address format\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

// The reference eibwrite-cgi.c requires the first hex byte to contain
// the A_GroupValue_Write APCI (bit 7 must be set).  A value without it is a
// client error: 400, no packet sent, never a silent "success".
TEST_F(WriteHandlerTest, WriteValueWithoutApciIsRejected) {
  WriteHandler handler(knxd_, sessions_);

  // v=0c6f — first byte 0x0C does not have bit 7 set
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=0c6f");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());

  // v=42 — single byte 0x42 does not have bit 7 set
  result = handler.handle("s=0&a=KNX:1/2/3&v=42");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

// Verifies that only A_GroupValue_Write (0x80) is accepted.
// A_GroupValue_Read (0x00) and A_GroupValue_Response (0x40) are rejected.
TEST_F(WriteHandlerTest, WriteValueWithReadApciIsRejected) {
  WriteHandler handler(knxd_, sessions_);

  // v=000c6f — first byte 0x00 = Read APCI, not Write
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=000c6f");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());

  // v=400c6f — first byte 0x40 = Response APCI, not Write
  result = handler.handle("s=0&a=KNX:1/2/3&v=400c6f");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, WriteDoesNotNeedLocalCache) {
  // After removing AddressCache, writes just send the packet.
  // knxd's built-in cache handles storage.
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("s=0&a=KNX:1/2/3&v=800c6f");
  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
}

TEST_F(WriteHandlerTest, DefaultNamespace) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("s=0&a=1/2/3&v=8042");

  EXPECT_EQ(result.http_status, 200);
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
}

TEST_F(WriteHandlerTest, SessionInvalidReturns401) {
  (void)sessions_.create_session(false);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=8042&s=nonexistent");
  EXPECT_EQ(result.http_status, 401);
  EXPECT_NE(result.body.find("\"error\":\"invalid session\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, AnonymousSessionOk) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=8042&s=0");
  EXPECT_EQ(result.http_status, 200);
  EXPECT_FALSE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, ValidSessionWrites) {
  auto sid = sessions_.create_session(false);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=8042&s=" + sid);
  EXPECT_EQ(result.http_status, 200);
  EXPECT_FALSE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, SendFailureReturns503) {
  knxd_.set_send_fail_count(1);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=8042");
  EXPECT_EQ(result.http_status, 503);
  EXPECT_NE(result.body.find("\"error\":\"write failed\""), std::string::npos);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, PartialFailureReturns503) {
  knxd_.set_send_fail_count(1);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=0&a=KNX:1/2/3&a=KNX:4/5/6&v=8042");
  EXPECT_EQ(result.http_status, 503);
  // A partially applied multi-address write must say so instead of claiming a
  // plain failure (or, worse, success).
  EXPECT_NE(result.body.find("\"error\":\"write partially failed\""), std::string::npos)
      << result.body;
  EXPECT_NE(result.body.find("\"failed\":1"), std::string::npos) << result.body;
  EXPECT_NE(result.body.find("\"total\":2"), std::string::npos) << result.body;
}

TEST_F(WriteHandlerTest, SendFailureClearedOnSubsequentCalls) {
  // After a failure, subsequent writes should work normally.
  knxd_.set_send_fail_count(1);

  WriteHandler handler(knxd_, sessions_);

  // First call fails
  auto result1 = handler.handle("s=0&a=KNX:1/2/3&v=8042");
  EXPECT_EQ(result1.http_status, 503);
  EXPECT_NE(result1.body.find("\"error\":\"write failed\""), std::string::npos);

  // Reset the mock and try again — should succeed
  knxd_.set_send_fail_count(0);

  auto result2 = handler.handle("s=0&a=KNX:1/2/3&v=8042");
  EXPECT_EQ(result2.http_status, 200);
  EXPECT_FALSE(knxd_.sent_packets().empty());
}

// ================================================================
// knxd availability: a failed write must tell the client *why* (and since
// when), so it can show "KNX connection lost" instead of a generic error.
// ================================================================

TEST_F(WriteHandlerTest, FailedWriteReportsTheKnxdOutage) {
  cache_.publish_bus_status(true, 1000);
  cache_.publish_bus_status(false, 1500);
  knxd_.set_send_fail_count(1);

  WriteHandler handler(knxd_, sessions_, &cache_);
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=8042");

  EXPECT_EQ(result.http_status, 503);
  EXPECT_NE(result.body.find("\"error\":\"write failed\""), std::string::npos) << result.body;
  EXPECT_NE(result.body.find("\"knxd\":{"), std::string::npos) << result.body;
  EXPECT_NE(result.body.find("\"ok\":false"), std::string::npos) << result.body;
  EXPECT_NE(result.body.find("\"since\":1500"), std::string::npos) << result.body;
  EXPECT_NE(result.body.find("\"lastOk\":1000"), std::string::npos) << result.body;
}

TEST_F(WriteHandlerTest, FailedWriteWithoutCacheStillReportsUnavailability) {
  knxd_.set_send_fail_count(1);

  WriteHandler handler(knxd_, sessions_);  // no cache attached
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=8042");

  EXPECT_EQ(result.http_status, 503);
  EXPECT_NE(result.body.find("\"knxd\":{\"ok\":false}"), std::string::npos) << result.body;
}

TEST_F(WriteHandlerTest, SuccessfulWriteHasNoKnxdBlock) {
  cache_.publish_bus_status(true, 1000);

  WriteHandler handler(knxd_, sessions_, &cache_);
  auto result = handler.handle("s=0&a=KNX:1/2/3&v=8042");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_EQ(result.body.find("\"knxd\""), std::string::npos) << result.body;
}

// ================================================================
// Value validation edge cases
// ================================================================

TEST_F(WriteHandlerTest, EmptyValueIsRejected) {
  // An empty `v` is not a value: it is a client error, not a silent success.
  WriteHandler handler(knxd_, sessions_);
  auto r = handler.handle("s=0&a=1/2/3&v=");
  EXPECT_EQ(r.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, ValueWithoutApciIsRejected) {
  WriteHandler handler(knxd_, sessions_);
  auto r = handler.handle("s=0&a=1/2/3&v=42");
  EXPECT_EQ(r.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, OverlongValueIsRejected) {
  WriteHandler handler(knxd_, sessions_);
  // 15 data bytes + APCI byte + the prepended 0x00 exceeds the frame limit.
  auto r = handler.handle("s=0&a=1/2/3&v=80000102030405060708090a0b0c0d0e0f");
  EXPECT_EQ(r.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

// 13 bytes of `v` (12 data octets) is the largest value a standard cEMI frame
// can carry: the handler and the TX ring must agree on that boundary, otherwise
// a knxd deployment accepts a value that every ip-* deployment answers 503.
TEST_F(WriteHandlerTest, ValueAtTheFrameLimitIsAccepted) {
  WriteHandler handler(knxd_, sessions_);
  auto r = handler.handle("s=0&a=1/2/3&v=800102030405060708090a0b0c");  // 13 bytes
  EXPECT_EQ(r.http_status, 200);
  ASSERT_EQ(knxd_.sent_packets().size(), 1U);
  EXPECT_EQ(knxd_.sent_packets()[0].apdu.size(), 14U);
}

TEST_F(WriteHandlerTest, ValueAboveTheFrameLimitIsRejected) {
  WriteHandler handler(knxd_, sessions_);
  auto r = handler.handle("s=0&a=1/2/3&v=800102030405060708090a0b0c0d");  // 14 bytes
  EXPECT_EQ(r.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}
