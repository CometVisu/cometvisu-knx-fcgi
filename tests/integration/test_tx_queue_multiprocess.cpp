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
 * @file test_tx_queue_multiprocess.cpp
 * @brief The shared TX ring under real multi-process pressure.
 *
 * The queue is the only place where worker processes and the cache reader
 * meet, so it is the one component where a lost, duplicated, reordered or
 * torn telegram silently corrupts the bus.  These tests use real fork()ed
 * producers and a real consumer, with the consumer deliberately slower than
 * the producers (a rate-limited bus), and with processes dying mid-operation.
 */

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

constexpr int kProducers = 4;
constexpr int kPerProducer = 500;

/// Telegram identity: producer index in byte 0, sequence in bytes 1..2.
Bytes Payload(int producer, int sequence) {
  return {0x00, 0x80, static_cast<std::uint8_t>(producer),
          static_cast<std::uint8_t>((sequence >> 8) & 0xFF),
          static_cast<std::uint8_t>(sequence & 0xFF)};
}

std::uint16_t Address(int producer, int sequence) {
  return static_cast<std::uint16_t>((producer << 11) | (sequence & 0x07FF));
}

/// Fork a producer that pushes its whole stream, retrying while the ring is
/// full (a real worker would answer 503 instead — the lossless path is what is
/// under test here).
pid_t SpawnProducer(SharedTxQueue* queue, int producer, int count, int delay_us) {
  const pid_t pid = ::fork();
  if (pid != 0) {
    return pid;
  }
  for (int i = 0; i < count; ++i) {
    while (!queue->push(Address(producer, i), Payload(producer, i))) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    queue->signal();
    if (delay_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }
  }
  std::_Exit(0);
}

/// Collect exactly @p expected telegrams, sleeping @p drain_delay_us per item
/// to emulate a rate-limited bus.
struct Collected {
  std::vector<std::pair<std::uint16_t, Bytes>> telegrams;
};

Collected Collect(SharedTxQueue& queue, int expected, int drain_delay_us, int timeout_ms) {
  Collected collected;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

  while (static_cast<int>(collected.telegrams.size()) < expected && Clock::now() < deadline) {
    queue.drain_notifications();
    std::uint16_t addr = 0;
    Bytes apdu;
    if (!queue.pop(addr, apdu)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    collected.telegrams.emplace_back(addr, apdu);
    if (drain_delay_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(drain_delay_us));
    }
  }
  return collected;
}

TEST(TxQueueMultiprocessTest, ConcurrentProducersLoseNothingAndKeepTheirOrder) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  std::vector<pid_t> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.push_back(SpawnProducer(&queue, p, kPerProducer, /*delay_us=*/0));
    ASSERT_GT(producers.back(), 0);
  }

  // The consumer is intentionally slower than the producers: the ring must
  // apply backpressure, not lose or mangle telegrams.
  const auto collected = Collect(queue, kProducers * kPerProducer, /*drain_delay_us=*/50,
                                 /*timeout_ms=*/60000);

  for (pid_t pid : producers) {
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status)) << "producer died";
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

  ASSERT_EQ(collected.telegrams.size(), static_cast<std::size_t>(kProducers * kPerProducer));

  // Every telegram arrived exactly once, with intact contents.
  std::set<std::pair<int, int>> seen;
  std::vector<int> last(kProducers, -1);
  for (const auto& [addr, apdu] : collected.telegrams) {
    ASSERT_EQ(apdu.size(), 5U);
    const int producer = apdu[2];
    const int sequence = (apdu[3] << 8) | apdu[4];
    ASSERT_GE(producer, 0);
    ASSERT_LT(producer, kProducers);
    EXPECT_EQ(addr, Address(producer, sequence)) << "address/payload mismatch (torn slot)";
    EXPECT_TRUE(seen.insert({producer, sequence}).second)
        << "duplicate telegram producer=" << producer << " seq=" << sequence;
    EXPECT_GT(sequence, last[producer]) << "order broken for producer " << producer;
    last[producer] = sequence;
  }
  EXPECT_EQ(queue.size(), 0U);
}

TEST(TxQueueMultiprocessTest, ProducerKilledMidStreamDoesNotBlockTheOthers) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  // A producer that dies inside its stream (SIGKILL, no cleanup).
  const pid_t victim = ::fork();
  ASSERT_GE(victim, 0);
  if (victim == 0) {
    for (int i = 0;; ++i) {
      if (!queue.push(Address(0, i), Payload(0, i))) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        continue;
      }
      queue.signal();
      if (i == 20) {
        ::raise(SIGKILL);  // dies while the ring is in use
      }
    }
  }

  pid_t survivor = SpawnProducer(&queue, 1, 200, 0);
  ASSERT_GT(survivor, 0);

  int status = 0;
  ASSERT_EQ(::waitpid(victim, &status, 0), victim);
  ASSERT_TRUE(WIFSIGNALED(status)) << "the victim must have been killed";

  // The surviving producer must complete: a dead peer cannot wedge the ring.
  ASSERT_EQ(::waitpid(survivor, &status, 0), survivor);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);

  // Everything that was queued is readable (no lock left behind).
  int drained = 0;
  std::uint16_t addr = 0;
  Bytes apdu;
  while (queue.pop(addr, apdu)) {
    ++drained;
  }
  EXPECT_GE(drained, 200) << "survivor's telegrams must all be present";
}

TEST(TxQueueMultiprocessTest, MutexOwnerDeathUnderConcurrentLoadRecovers) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  std::vector<pid_t> producers;
  for (int p = 0; p < 2; ++p) {
    producers.push_back(SpawnProducer(&queue, p, 200, /*delay_us=*/50));
  }

  // Hammer the queue with a process that dies while holding the mutex.
  const pid_t crasher = ::fork();
  ASSERT_GE(crasher, 0);
  if (crasher == 0) {
    ::pthread_mutex_lock(&queue.data()->mutex);
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(crasher, &status, 0), crasher);

  const auto collected = Collect(queue, 2 * 200, 0, 30000);
  for (pid_t pid : producers) {
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
  EXPECT_EQ(collected.telegrams.size(), 400U)
      << "EOWNERDEAD recovery must not lose or block telegrams";
}

TEST(TxQueueMultiprocessTest, FullQueueRejectsCleanlyUnderLoad) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  // No consumer at all: the ring fills up and every further push must fail
  // without corrupting what is already there.
  const pid_t producer = ::fork();
  ASSERT_GE(producer, 0);
  if (producer == 0) {
    int rejected = 0;
    for (int i = 0; i < 400; ++i) {
      if (!queue.push(Address(2, i), Payload(2, i))) {
        ++rejected;
      }
    }
    std::_Exit(rejected > 0 ? 0 : 1);  // needs at least one rejection
  }
  int status = 0;
  ASSERT_EQ(::waitpid(producer, &status, 0), producer);
  EXPECT_EQ(WEXITSTATUS(status), 0) << "a full queue must reject, not block";

  // Exactly capacity-1 telegrams are queued, in order, all intact.
  int index = 0;
  std::uint16_t addr = 0;
  Bytes apdu;
  while (queue.pop(addr, apdu)) {
    ASSERT_EQ(apdu.size(), 5U);
    const int sequence = (apdu[3] << 8) | apdu[4];
    EXPECT_EQ(sequence, index) << "rejection must not tear the ring";
    ++index;
  }
  EXPECT_EQ(index, static_cast<int>(kTxQueueCapacity) - 1);
}

TEST(TxQueueMultiprocessTest, ConsumerRestartKeepsQueuedTelegrams) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x01}));
  ASSERT_TRUE(queue.push(0x0A04, {0x00, 0x80, 0x02}));

  // The reader process dies before sending anything.
  const pid_t reader = ::fork();
  ASSERT_GE(reader, 0);
  if (reader == 0) {
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(reader, &status, 0), reader);

  // A replacement reader finds the old telegrams untouched, in order.
  std::uint16_t addr = 0;
  Bytes apdu;
  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  ASSERT_TRUE(queue.pop(addr, apdu));
  EXPECT_EQ(addr, 0x0A04);
  EXPECT_FALSE(queue.pop(addr, apdu));
}

}  // namespace
}  // namespace cvknx
