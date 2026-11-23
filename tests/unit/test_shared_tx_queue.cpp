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

// ================================================================
// Pending group reads (`t=0`) live in their own, lower-priority ring
// ================================================================

TEST(SharedTxQueueTest, ReadRequestsUseTheirOwnRing) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  ASSERT_TRUE(queue.push_read(0x0A04));
  EXPECT_EQ(queue.size(), 1U);
  EXPECT_EQ(queue.read_size(), 1U);

  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.peek(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  ASSERT_TRUE(queue.peek_read(addr));
  EXPECT_EQ(addr, 0x0A04);

  // Removing a write must not touch the read ring, and vice versa.
  queue.pop_drop();
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.read_size(), 1U);
  queue.pop_read_drop();
  EXPECT_EQ(queue.read_size(), 0U);
  queue.pop_read_drop();  // popping an empty ring is a no-op
  EXPECT_EQ(queue.read_size(), 0U);
}

TEST(SharedTxQueueTest, APendingReadIsNotQueuedTwice) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  EXPECT_TRUE(queue.push_read(0x0A03));
  EXPECT_TRUE(queue.push_read(0x0A03));  // already in flight
  EXPECT_EQ(queue.read_size(), 1U);
  EXPECT_TRUE(queue.push_read(0x0B04));
  EXPECT_EQ(queue.read_size(), 2U);
}

TEST(SharedTxQueueTest, ReadRingIsBoundedIndependently) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  const auto base = static_cast<std::uint16_t>(0x1000);
  for (std::size_t i = 0; i < kReadQueueCapacity - 1; ++i) {
    ASSERT_TRUE(queue.push_read(static_cast<std::uint16_t>(base + i)));
  }
  EXPECT_EQ(queue.read_size(), kReadQueueCapacity - 1);
  EXPECT_FALSE(queue.push_read(0x0A03));  // full
  // The write ring is unaffected by a full read ring.
  EXPECT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
}

/// A cold-cache client may ask for up to 4096 addresses in one `a` parameter
/// (`QueryString::kMaxValuesPerKey`) and sends `t=0` only once — so the whole
/// list must fit, or the tail of the visualization stays empty forever.
TEST(SharedTxQueueTest, AMaximalAddressListFitsInTheReadRing) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  constexpr std::size_t kMaxAddressesPerRequest = 4096;
  for (std::size_t i = 0; i < kMaxAddressesPerRequest; ++i) {
    ASSERT_TRUE(queue.push_read(static_cast<std::uint16_t>(0x1000 + i)))
        << "address " << i << " of a maximal request was rejected";
  }
  EXPECT_EQ(queue.read_size(), kMaxAddressesPerRequest);
}

TEST(SharedTxQueueTest, ReadRequestsAreSharedAcrossAttachedInstances) {
  SharedTxQueue writer;
  ASSERT_TRUE(writer.create());
  SharedTxQueue reader;
  reader.attach(writer.data());

  ASSERT_TRUE(writer.push_read(0x0A03));
  EXPECT_EQ(reader.read_size(), 1U);
  std::uint16_t addr = 0;
  ASSERT_TRUE(reader.peek_read(addr));
  EXPECT_EQ(addr, 0x0A03);
  reader.pop_read_drop();
  EXPECT_EQ(writer.read_size(), 0U);
}

/// The read ring wraps like any ring: the count must stay correct once the
/// write index has wrapped past the end.
TEST(SharedTxQueueTest, ReadRingWrapsAroundTheEnd) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  ASSERT_TRUE(queue.push_read(0x0A03));
  queue.pop_read_drop();  // head = 1
  for (std::size_t i = 0; i < kReadQueueCapacity - 1; ++i) {
    ASSERT_TRUE(queue.push_read(static_cast<std::uint16_t>(0x1000 + i)));
  }
  // tail wrapped to 0, head is 1: the count is the wrapped distance.
  EXPECT_EQ(queue.read_size(), kReadQueueCapacity - 1);
  std::uint16_t addr = 0;
  ASSERT_TRUE(queue.peek_read(addr));
  EXPECT_EQ(addr, 0x1000);  // FIFO order survives the wrap
}

/// An uninitialized ring reports "nothing" instead of crashing; this is the
/// process that was handed a queue whose creation failed.
TEST(SharedTxQueueTest, UninitializedReadRingIsEmpty) {
  SharedTxQueue queue;
  EXPECT_FALSE(queue.push_read(0x0A03));
  std::uint16_t addr = 0;
  EXPECT_FALSE(queue.peek_read(addr));
  EXPECT_EQ(queue.read_size(), 0U);
  queue.pop_read_drop();  // no-op
}

}  // namespace
}  // namespace cvknx
