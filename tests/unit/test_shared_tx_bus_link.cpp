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
 * @file test_shared_tx_bus_link.cpp
 * @brief Worker-side writes: enqueue when the bus is up, fail when it is not.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/bus/shared_tx_bus_link.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

class SharedTxBusLinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(queue_.create());
    ASSERT_TRUE(cache_.create());
    cache_.publish_bus_status(true);
  }

  SharedTxQueue queue_;
  SharedGroupCache cache_;
};

TEST_F(SharedTxBusLinkTest, EnqueuesAndSignals) {
  SharedTxBusLink link(queue_, cache_);
  ASSERT_TRUE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(queue_.size(), 1U);

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue_.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x42}));
}

TEST_F(SharedTxBusLinkTest, RejectsWriteDuringOutage) {
  cache_.publish_bus_status(false);
  SharedTxBusLink link(queue_, cache_);

  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  // A rejected write must not be queued: it would be sent after the reconnect
  // and the client would have received a 200 for a telegram that never went out.
  EXPECT_EQ(queue_.size(), 0U);
}

TEST_F(SharedTxBusLinkTest, FullQueueFailsTheWrite) {
  SharedTxBusLink link(queue_, cache_);
  for (std::size_t i = 0; i < kTxQueueCapacity - 1; ++i) {
    ASSERT_TRUE(link.send_group_packet(static_cast<std::uint16_t>(0x1000 + i), {0x00, 0x80, 0x01}));
  }
  EXPECT_FALSE(link.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
}

TEST_F(SharedTxBusLinkTest, NeverReceivesAndHasNoFd) {
  SharedTxBusLink link(queue_, cache_);
  std::uint16_t addr = 0;
  Bytes apdu;
  EXPECT_FALSE(link.poll_group_telegram(addr, apdu));
  EXPECT_EQ(link.get_fd(), -1);
  EXPECT_EQ(link.wait_for_activity(1), BusLink::WaitResult::Timeout);
  EXPECT_TRUE(link.connect());
  EXPECT_TRUE(link.is_connected());
}

}  // namespace
}  // namespace cvknx
