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
 * @file test_cache_reader_backpressure.cpp
 * @brief Cache reader with a slow / failing bus and a bursty producer.
 *
 * These are the situations a real bus creates: it answers slowly, it stops
 * answering for a while, and writes arrive in bursts that are faster than the
 * bus can take them.  Nothing may be lost, duplicated or reordered, the loop
 * must stay stoppable, and it must never spin.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/bus/bus_link.h"
#include "../../src/bus/cache_reader.h"
#include "../../src/bus/shared_tx_bus_link.h"
#include "../../src/handlers/write_handler.h"
#include "../../src/state/session_store.h"
#include "../../src/state/shared_group_cache.h"
#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

/// A bus that answers slowly, fails on demand and records what it received.
class ScriptedBusLink : public BusLink {
public:
  explicit ScriptedBusLink(int send_delay_us) : send_delay_us_(send_delay_us) {}

  [[nodiscard]] bool connect() override {
    connected_ = true;
    ++connect_calls_;
    return true;
  }
  void disconnect() override { connected_ = false; }
  [[nodiscard]] bool is_connected() const override { return connected_; }

  [[nodiscard]] bool send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) override {
    const int attempt = send_attempts_++;
    if (send_delay_us_ > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(send_delay_us_));
    }
    if (silent_ || fail_indices_.count(attempt) != 0) {
      return false;
    }
    sent_.emplace_back(group_addr, apdu);
    return true;
  }

  [[nodiscard]] bool poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) override {
    (void)out_group_addr;
    (void)out_apdu;
    return false;
  }
  [[nodiscard]] int get_fd() const override { return -1; }
  [[nodiscard]] std::uint64_t get_telegram_count() const override { return 0; }
  void set_nonblocking(bool enable) override { (void)enable; }
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return WaitResult::Timeout;
  }
  [[nodiscard]] std::string label() const override { return "scripted bus"; }

  /// Make the N-th send attempt (0-based) fail.
  void fail_send_at(int index) { fail_indices_.insert(index); }
  void clear_failures() { fail_indices_.clear(); }
  /// The bus stops answering entirely (device gone, cable pulled).
  void set_silent(bool silent) { silent_ = silent; }

  [[nodiscard]] const std::vector<std::pair<std::uint16_t, Bytes>>& sent() const { return sent_; }
  [[nodiscard]] int connect_calls() const { return connect_calls_; }

private:
  int send_delay_us_ = 0;
  bool connected_ = false;
  int send_attempts_ = 0;
  int connect_calls_ = 0;
  bool silent_ = false;
  std::set<int> fail_indices_;
  std::vector<std::pair<std::uint16_t, Bytes>> sent_;
};

CacheReaderOptions FastOptions() {
  CacheReaderOptions options;
  options.wait_timeout_ms = 5;
  options.reconnect_backoff_ms = 20;
  options.max_reconnect_backoff_ms = 20;
  options.sleep_slice_ms = 2;
  options.max_tx_per_sec = 0;
  return options;
}

Bytes Value(int sequence) {
  return {0x00, 0x80, static_cast<std::uint8_t>((sequence >> 8) & 0xFF),
          static_cast<std::uint8_t>(sequence & 0xFF)};
}

int SequenceOf(const Bytes& apdu) {
  return (apdu.at(2) << 8) | apdu.at(3);
}

TEST(CacheReaderBackpressureTest, SlowBusDrainsBurstInOrder) {
  ScriptedBusLink bus(/*send_delay_us=*/200);  // rate-limited bus
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  constexpr int kCount = 60;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A00 + i), Value(i)));
  }
  queue.signal();

  run_cache_reader(bus, cache, &queue, FastOptions(),
                   [&bus] { return bus.sent().size() >= static_cast<std::size_t>(kCount); });

  ASSERT_EQ(bus.sent().size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(bus.sent()[static_cast<std::size_t>(i)].first, 0x0A00 + i) << "order broken";
    EXPECT_EQ(SequenceOf(bus.sent()[static_cast<std::size_t>(i)].second), i);
  }
  EXPECT_EQ(queue.size(), 0U) << "a slow bus must drain the backlog completely";
}

TEST(CacheReaderBackpressureTest, FailedSendKeepsBacklogQueuedInOrder) {
  ScriptedBusLink bus(/*send_delay_us=*/0);
  bus.fail_send_at(1);  // the second telegram fails once (bus hiccup)
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  constexpr int kCount = 5;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A00 + i), Value(i)));
  }
  queue.signal();

  run_cache_reader(bus, cache, &queue, FastOptions(),
                   [&bus] { return bus.sent().size() >= static_cast<std::size_t>(kCount); });

  ASSERT_EQ(bus.sent().size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(SequenceOf(bus.sent()[static_cast<std::size_t>(i)].second), i)
        << "a failed send must not reorder or drop the backlog";
  }
  EXPECT_EQ(queue.size(), 0U);
}

TEST(CacheReaderBackpressureTest, LongOutageKeepsEveryTelegramUntilRecovery) {
  ScriptedBusLink bus(/*send_delay_us=*/0);
  bus.set_silent(true);  // the bus stops answering entirely
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  constexpr int kCount = 8;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A00 + i), Value(i)));
  }
  queue.signal();

  // During the outage nothing is delivered but nothing is dropped either.
  const auto outage_deadline = Clock::now() + std::chrono::milliseconds(100);
  run_cache_reader(bus, cache, &queue, FastOptions(),
                   [&outage_deadline] { return Clock::now() >= outage_deadline; });
  EXPECT_TRUE(bus.sent().empty());
  EXPECT_EQ(queue.size(), static_cast<std::size_t>(kCount));

  // Recovery: the backlog goes out in the original order.
  bus.set_silent(false);
  run_cache_reader(bus, cache, &queue, FastOptions(),
                   [&bus] { return bus.sent().size() >= static_cast<std::size_t>(kCount); });

  ASSERT_EQ(bus.sent().size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(SequenceOf(bus.sent()[static_cast<std::size_t>(i)].second), i);
  }
}

TEST(CacheReaderBackpressureTest, StopsPromptlyWhileBacklogExists) {
  // A slow bus plus a large backlog must not delay shutdown: the supervisor
  // sends SIGTERM and expects the reader to be gone quickly.
  ScriptedBusLink bus(/*send_delay_us=*/20000);  // 20 ms per telegram
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A00 + i), Value(i)));
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(60);
  const auto start = Clock::now();
  run_cache_reader(bus, cache, &queue, FastOptions(),
                   [&deadline] { return Clock::now() >= deadline; });
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();

  EXPECT_LT(elapsed, 1000) << "the reader must honour the stop predicate between telegrams";
  EXPECT_LT(bus.sent().size(), 100U) << "it stopped before draining the whole backlog";
  EXPECT_GT(queue.size(), 0U) << "unsent telegrams are kept, not dropped";
}

// ---------------------------------------------------------------------------
// Concurrent request handlers pushing into the same queue
// ---------------------------------------------------------------------------

TEST(WriteHandlerConcurrencyTest, ConcurrentWritesAreAllQueuedExactlyOnce) {
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  cache.publish_bus_status(true);

  SharedTxBusLink link(queue, cache);
  SessionStore sessions;
  WriteHandler handler(link, sessions, &cache, "bus");

  constexpr int kThreads = 8;
  constexpr int kPerThread = 20;  // 8 x 20 < capacity, see FullQueue tests
  std::vector<std::thread> clients;
  std::vector<int> statuses(kThreads, 0);
  for (int t = 0; t < kThreads; ++t) {
    clients.emplace_back([&handler, &statuses, t] {
      int failures = 0;
      for (int i = 0; i < kPerThread; ++i) {
        // Address is unique per (client, sequence): a torn or crossed enqueue
        // is visible in the final queue contents.
        char value[8] = {};
        std::snprintf(value, sizeof(value), "80%02x", i & 0xFF);
        const std::string query = "s=0&a=1/2/" + std::to_string(t * kPerThread + i) + "&v=" + value;
        const auto result = handler.handle(query);
        if (result.http_status != 200) {
          ++failures;
        }
      }
      statuses[static_cast<std::size_t>(t)] = failures;
    });
  }
  for (auto& client : clients) {
    client.join();
  }

  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(statuses[static_cast<std::size_t>(t)], 0) << "client " << t << " saw a failed write";
  }
  EXPECT_EQ(queue.size(), static_cast<std::size_t>(kThreads * kPerThread));

  std::set<std::uint16_t> addresses;
  std::uint16_t addr = 0;
  Bytes apdu;
  while (queue.pop(addr, apdu)) {
    EXPECT_TRUE(addresses.insert(addr).second) << "duplicate enqueue for " << addr;
    EXPECT_EQ(apdu.size(), 3U);
  }
  EXPECT_EQ(addresses.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace
}  // namespace cvknx
