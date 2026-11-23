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
 * @file test_shared_tx_queue.cpp
 * @brief The shared TX ring must be a lossless, bounded, multi-process FIFO.
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;

TEST(SharedTxQueueTest, PushPopFifo) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  ASSERT_TRUE(queue.push(0x0A04, {0x00, 0x80, 0x0C, 0x6F}));
  EXPECT_EQ(queue.size(), 2U);

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x42}));

  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A04);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));

  EXPECT_FALSE(queue.pop(addr, apdu));
  EXPECT_EQ(queue.size(), 0U);
}

TEST(SharedTxQueueTest, FifoAcrossAttachedInstances) {
  SharedTxQueue writer;
  ASSERT_TRUE(writer.create());

  SharedTxQueue reader;
  reader.attach(writer.data());

  ASSERT_TRUE(writer.push(0x0A03, {0x00, 0x80, 0x01}));
  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(reader.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x01}));
}

TEST(SharedTxQueueTest, FullQueueRejectsPushAndKeepsContent) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  // The ring keeps one slot free to distinguish full from empty.
  for (std::size_t i = 0; i < kTxQueueCapacity - 1; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x1000 + i), {0x00, 0x80, 0x01}))
        << "push " << i;
  }
  EXPECT_FALSE(queue.push(0x0A03, {0x00, 0x80, 0x42}));

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(addr, 0x1000);  // oldest entry survived
  EXPECT_EQ(queue.size(), kTxQueueCapacity - 2);
}

TEST(SharedTxQueueTest, PayloadTooLongRejected) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  const Bytes too_long(kMaxTxValueBytes + 1, 0x00);
  EXPECT_FALSE(queue.push(0x0A03, too_long));
  EXPECT_EQ(queue.size(), 0U);
}

TEST(SharedTxQueueTest, EmptyApduRejected) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  EXPECT_FALSE(queue.push(0x0A03, {}));
}

TEST(SharedTxQueueTest, MaximumPayloadFits) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  const Bytes max_payload(kMaxTxValueBytes, 0xAA);
  ASSERT_TRUE(queue.push(0x0A03, max_payload));
  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(apdu, max_payload);
}

TEST(SharedTxQueueTest, SignalEventfdIncrements) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_GE(queue.eventfd(), 0);

  queue.signal();
  queue.signal();

  std::uint64_t value = 0;
  ASSERT_EQ(::read(queue.eventfd(), &value, sizeof(value)), static_cast<ssize_t>(sizeof(value)));
  EXPECT_EQ(value, 2U);
}

TEST(SharedTxQueueTest, DrainNotificationsEmptiesEventfd) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  queue.signal();
  queue.drain_notifications();

  // Non-blocking eventfd: reading an empty counter fails with EAGAIN.
  std::uint64_t value = 0;
  EXPECT_LT(::read(queue.eventfd(), &value, sizeof(value)), 0);
}

TEST(SharedTxQueueTest, RobustMutexRecoversWhenOwnerDies) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  // Simulate a process dying while holding the lock: a child locks and exits.
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::pthread_mutex_lock(&queue.data()->mutex);
    ::_exit(0);  // dies without unlocking
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);

  // The next push must recover the mutex (EOWNERDEAD → consistent) instead of
  // blocking forever.
  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_EQ(queue.size(), 1U);
}

/// A process that dies mid-push can leave a length byte that does not match the
/// slot; dequeuing must clamp it instead of copying into the next slots.
TEST(SharedTxQueueTest, ATornLengthIsClampedWhenDequeued) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  queue.data()->slots[0].len = 200;

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.peek(addr, apdu));
  EXPECT_EQ(apdu.size(), kMaxTxValueBytes);

  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(apdu.size(), kMaxTxValueBytes);
}

}  // namespace
}  // namespace cvknx
