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
 * @file test_cache_reader.cpp
 * @brief Tests for the knxd -> SharedGroupCache reader loop.
 *
 * The loop must survive an absent or dying knxd: it waits between reconnect
 * attempts (never spinning), retries forever, and resumes pushing telegrams
 * once knxd comes back.
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../../mocks/mock_bus_link.h"
#include "../../src/bus/cache_reader.h"
#include "../../src/state/shared_group_cache.h"
#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Clock = std::chrono::steady_clock;

/// Options with tiny delays so tests run fast while still measuring real sleeps.
CacheReaderOptions FastOptions() {
  CacheReaderOptions options;
  options.wait_timeout_ms = 5;
  options.reconnect_backoff_ms = 20;
  options.max_reconnect_backoff_ms = 20;
  options.sleep_slice_ms = 2;
  // Tests that do not exercise the rate limit must not wait for it.
  options.max_tx_per_sec = 0;
  return options;
}

int64_t ElapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

TEST(CacheReaderTest, StopsImmediatelyWithoutTouchingKnxdWhenAlreadyStopped) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [] { return true; });

  EXPECT_EQ(knxd.connect_calls(), 0);
  EXPECT_EQ(knxd.wait_calls(), 0);
}

TEST(CacheReaderTest, WaitsBetweenReconnectAttemptsWhenKnxdIsMissing) {
  MockBusLink knxd;
  knxd.set_connection_success(false);  // knxd is not there (no /run/knx)
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // Stop after the fourth failed connect attempt.
  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // Three backoff sleeps of 20 ms must have happened — a busy loop would
  // return in well under a millisecond and fail here.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, ReconnectsWhenTheConnectionIsLost) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  knxd.set_wait_error_count(3);  // first three waits report a dead connection
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // 1 initial connect + 3 reconnects.
  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // One backoff sleep per lost connection — again, no busy loop.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, DoesNotReconnectWhileTheConnectionIsHealthy) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.wait_calls() >= 20; });

  EXPECT_EQ(knxd.connect_calls(), 1);
  EXPECT_GE(knxd.wait_calls(), 20);
}

TEST(CacheReaderTest, PushesReceivedTelegramsIntoSharedCache) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  // APDU for A_GroupValue_Write with value 0x42.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&cache] { return cache.get(0x0A03).has_value(); });

  const auto value = cache.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  ASSERT_EQ(value->size(), 1U);
  EXPECT_EQ((*value)[0], 0x42);
  EXPECT_EQ(cache.position(), 1U);
}

TEST(CacheReaderTest, IgnoresGroupValueReadTelegrams) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  // A_GroupValue_Read has no value — it must not enter the cache.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x00});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&knxd] { return knxd.wait_calls() >= 5; });

  EXPECT_FALSE(cache.get(0x0A03).has_value());
  EXPECT_EQ(cache.position(), 0U);
}

// ================================================================
// The reader publishes the knxd connection state for /r and /w.
// ================================================================

TEST(CacheReaderTest, PublishesAvailabilityWhileConnected) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&knxd] { return knxd.wait_calls() >= 3; });

  const auto status = cache.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_GT(status.last_ok, 0U) << "last successful connection must be stamped";
  EXPECT_EQ(status.down_since, 0U);
}

TEST(CacheReaderTest, PublishesOutageWhenKnxdIsMissing) {
  MockBusLink knxd;
  knxd.set_connection_success(false);  // no /run/knx
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, nullptr, FastOptions(),
                   [&knxd] { return knxd.connect_calls() >= 2; });

  const auto status = cache.bus_status();
  EXPECT_FALSE(status.available);
  EXPECT_GT(status.down_since, 0U) << "the outage start must be stamped";
}

TEST(CacheReaderTest, PublishesOutageWhileDisconnected) {
  MockBusLink knxd;
  knxd.set_connection_success(true);
  knxd.set_wait_error_count(1);  // connection dies on the first wait
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  // Stop as soon as the reader has connected once and then reported the
  // outage (before it reconnects) — the initial "unavailable" publish must
  // not end the loop early.
  bool saw_connected = false;
  run_cache_reader(knxd, cache, nullptr, FastOptions(), [&cache, &saw_connected] {
    const auto status = cache.bus_status();
    if (status.available) {
      saw_connected = true;
      return false;
    }
    return saw_connected && status.down_since > 0;
  });

  const auto status = cache.bus_status();
  EXPECT_TRUE(saw_connected);
  EXPECT_FALSE(status.available);
  EXPECT_GT(status.down_since, 0U);
  EXPECT_GT(status.last_ok, 0U) << "the last working connection must stay reported";
}

// ---- Shared TX queue integration ----

TEST(CacheReaderTest, DrainsTxQueueBeforePolling) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());
  ASSERT_TRUE(tx.push(0x0A03, {0x00, 0x80, 0x42}));

  run_cache_reader(knxd, cache, &tx, FastOptions(),
                   [&knxd] { return !knxd.sent_packets().empty(); });

  ASSERT_EQ(knxd.sent_packets().size(), 1U);
  EXPECT_EQ(knxd.sent_packets()[0].group_addr, 0x0A03);
  EXPECT_EQ(tx.size(), 0U);
}

TEST(CacheReaderTest, TxQueueDrainedWhenLinkIdle) {
  MockBusLink knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());

  CacheReaderOptions options = FastOptions();
  // Long slice: only the eventfd can end the wait this quickly.
  options.wait_timeout_ms = 5000;

  std::thread producer([&tx] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)tx.push(0x0A03, {0x00, 0x80, 0x42});
    tx.signal();
  });

  const auto start = Clock::now();
  run_cache_reader(knxd, cache, &tx, options, [&knxd] { return !knxd.sent_packets().empty(); });
  const int64_t elapsed = ElapsedMs(start);
  producer.join();

  EXPECT_LT(elapsed, 1000) << "the eventfd must wake the loop, not the poll slice";
  ASSERT_EQ(knxd.sent_packets().size(), 1U);
}

TEST(CacheReaderTest, TxIsNotReplayedWhileDisconnected) {
  // The link cannot send: the telegram must stay queued (no loss) instead of
  // being reported as sent, and the loop must not spin.
  MockBusLink knxd;
  knxd.set_send_fail_count(1000);  // link is up but every send fails
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue tx;
  ASSERT_TRUE(tx.create());
  ASSERT_TRUE(tx.push(0x0A03, {0x00, 0x80, 0x42}));

  const auto deadline = Clock::now() + std::chrono::milliseconds(200);
  run_cache_reader(knxd, cache, &tx, FastOptions(),
                   [&deadline] { return Clock::now() >= deadline; });

  EXPECT_TRUE(knxd.sent_packets().empty());
  EXPECT_EQ(tx.size(), 1U) << "unsent telegrams must not be dropped";
}

}  // namespace

/// Minimal BusLink with a real file descriptor so that the poll() paths of
/// wait_for_activity() can be exercised (MockBusLink has no fd).
class FdLink : public BusLink {
public:
  explicit FdLink(int fd) : fd_(fd) {}
  [[nodiscard]] bool connect() override { return true; }
  void disconnect() override {}
  [[nodiscard]] bool is_connected() const override { return true; }
  [[nodiscard]] bool send_group_packet(uint16_t, const std::vector<uint8_t>&) override {
    return true;
  }
  [[nodiscard]] bool poll_group_telegram(uint16_t&, std::vector<uint8_t>&) override {
    return false;
  }
  [[nodiscard]] int get_fd() const override { return fd_; }
  [[nodiscard]] uint64_t get_telegram_count() const override { return 0; }
  void set_nonblocking(bool) override {}
  [[nodiscard]] WaitResult wait_for_activity(int) override { return WaitResult::Timeout; }
  [[nodiscard]] std::string label() const override { return "fd link"; }

private:
  int fd_;
};

// ================================================================
// wait_for_activity(): every wake-up source
// ================================================================

TEST(CacheReaderWaitTest, PollsOnlyTheLinkWithoutATxQueue) {
  MockBusLink link;
  ASSERT_TRUE(link.connect("/run/knx"));
  EXPECT_EQ(detail::wait_for_activity(link, nullptr, 5), detail::Activity::Timeout);

  link.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  EXPECT_EQ(detail::wait_for_activity(link, nullptr, 5), detail::Activity::Data);

  link.disconnect();
  EXPECT_EQ(detail::wait_for_activity(link, nullptr, 5), detail::Activity::Error);
}

TEST(CacheReaderWaitTest, TreatsAQueueWithoutEventfdLikeNoQueue) {
  MockBusLink link;
  ASSERT_TRUE(link.connect("/run/knx"));
  SharedTxQueue uninitialised;  // eventfd() == -1
  ASSERT_LT(uninitialised.eventfd(), 0);

  EXPECT_EQ(detail::wait_for_activity(link, &uninitialised, 5), detail::Activity::Timeout);
  link.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  EXPECT_EQ(detail::wait_for_activity(link, &uninitialised, 5), detail::Activity::Data);
  link.disconnect();
  EXPECT_EQ(detail::wait_for_activity(link, &uninitialised, 5), detail::Activity::Error);
}

TEST(CacheReaderWaitTest, WakesOnAQueuedTelegramWhenTheLinkHasNoFd) {
  MockBusLink link;  // get_fd() returns -1
  ASSERT_TRUE(link.connect("/run/knx"));
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());

  EXPECT_EQ(detail::wait_for_activity(link, &queue, 5), detail::Activity::Timeout);

  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x42}));
  queue.signal();
  EXPECT_EQ(detail::wait_for_activity(link, &queue, 50), detail::Activity::Tx);
}

TEST(CacheReaderWaitTest, ReportsLinkDataAndErrorsWhenTheLinkHasAnFd) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  FdLink link(fds[0]);

  // Nothing to read: only the timeout path remains.
  EXPECT_EQ(detail::wait_for_activity(link, &queue, 20), detail::Activity::Timeout);

  ASSERT_EQ(::write(fds[1], "x", 1), 1);
  EXPECT_EQ(detail::wait_for_activity(link, &queue, 100), detail::Activity::Data);

  // Peer closed: poll() reports POLLIN|POLLHUP — the link is gone.
  ::close(fds[1]);
  EXPECT_EQ(detail::wait_for_activity(link, &queue, 100), detail::Activity::Error);

  ::close(fds[0]);
}

// ================================================================
// Interruptible sleep
// ================================================================

TEST(CacheReaderSleepTest, SleepsTheFullDurationWithoutAStopPredicate) {
  const auto start = Clock::now();
  detail::sleep_interruptible(20, 5, {});
  EXPECT_GE(ElapsedMs(start), 15);
}

TEST(CacheReaderSleepTest, ReturnsEarlyWhenStopped) {
  const auto start = Clock::now();
  detail::sleep_interruptible(5000, 5, [] { return true; });
  EXPECT_LT(ElapsedMs(start), 1000);
}

TEST(CacheReaderTest, DropsTelegramsWhoseApduCannotBeParsed) {
  MockBusLink link;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  link.enqueue_telegram(0x0A03, {0x00});  // too short for an APDU

  int iterations = 0;
  CacheReaderOptions options = FastOptions();
  // Give the loop enough slices to drain the telegram, then stop.
  run_cache_reader(link, cache, nullptr, options, [&iterations] { return ++iterations > 150; });

  EXPECT_FALSE(cache.get(0x0A03).has_value());
}

TEST(CacheReaderStopTest, EmptyPredicateNeverStops) {
  EXPECT_FALSE(detail::stop_requested({}));
}

TEST(CacheReaderStopTest, PredicateIsEvaluated) {
  bool flag = false;
  EXPECT_FALSE(detail::stop_requested([&flag] { return flag; }));
  flag = true;
  EXPECT_TRUE(detail::stop_requested([&flag] { return flag; }));
}

TEST(CacheReaderTest, RetryWarningsAreRateLimited) {
  MockBusLink link;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  link.set_connect_fail_count(1000);  // never connects

  CacheReaderOptions options = FastOptions();
  options.retry_log_interval_ms = 0;  // never suppress a warning
  run_cache_reader(link, cache, nullptr, options, [&link] { return link.connect_calls() >= 3; });

  // Several failed attempts happened, each passing the rate limit.
  EXPECT_GE(link.connect_calls(), 2);
  EXPECT_FALSE(cache.bus_status().available);
}
/// Without a TX queue the reader polls the link itself and must map every
/// WaitResult correctly.
TEST(CacheReaderTest, WithoutATxQueueTheLinkResultIsMapped) {
  MockBusLink link;
  ASSERT_TRUE(link.connect());

  // Nothing to read: a plain timeout, not an error.
  EXPECT_EQ(detail::wait_for_activity(link, nullptr, 5), detail::Activity::Timeout);

  // A queue that was attached (no inherited eventfd) takes the same path.
  MockBusLink link2;
  ASSERT_TRUE(link2.connect());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  SharedTxQueue attached_to_the_region;
  attached_to_the_region.attach(queue.data());
  EXPECT_LT(attached_to_the_region.eventfd(), 0);
  EXPECT_EQ(detail::wait_for_activity(link2, &attached_to_the_region, 5),
            detail::Activity::Timeout);
}

/// A send that kills the link (as the real transports do) must be noticed by
/// the loop *before* it blocks again, otherwise the reconnect never happens.
TEST(CacheReaderTest, ADeadLinkAfterAFailedSendTriggersAReconnect) {
  MockBusLink link;
  link.set_disconnect_on_send_failure(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x01}));
  link.set_send_fail_count(1);

  CacheReaderOptions options = FastOptions();
  const auto start = Clock::now();
  run_cache_reader(link, cache, &queue, options,
                   [&link, start] { return link.connect_calls() >= 2 || ElapsedMs(start) > 500; });

  EXPECT_GE(link.connect_calls(), 2) << "the loop did not reconnect after the failed send";
}

// ===========================================================================
// TX pacing: the transport's minimum send interval is a *wait*, not a sleep
// ===========================================================================

TEST(CacheReaderPacing, PacesSendsAndReportsTheRemainingDelay) {
  MockBusLink link;
  ASSERT_TRUE(link.connect());
  link.set_min_send_interval_ms(50);

  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A01, {0x00, 0x80, 0x01}));
  ASSERT_TRUE(queue.push(0x0A02, {0x00, 0x80, 0x02}));

  auto next_send_at = Clock::time_point{};
  const auto first = detail::drain_tx_queue(link, queue, {}, 8, 50, next_send_at);
  EXPECT_EQ(first.sent, 1) << "one telegram per interval";
  EXPECT_FALSE(first.failed);
  EXPECT_EQ(queue.size(), 1U) << "the paced telegram must stay queued";

  // Immediately afterwards the interval has not elapsed: nothing is sent and
  // the caller is told how long to wait (it must not spin).
  const auto second = detail::drain_tx_queue(link, queue, {}, 8, 50, next_send_at);
  EXPECT_EQ(second.sent, 0);
  EXPECT_GT(second.wait_ms, 0);
  EXPECT_EQ(link.sent_packets().size(), 1U);

  // After the interval the next telegram goes out.
  std::this_thread::sleep_for(std::chrono::milliseconds(55));
  const auto third = detail::drain_tx_queue(link, queue, {}, 8, 50, next_send_at);
  EXPECT_EQ(third.sent, 1);
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(link.sent_packets().size(), 2U);
}

TEST(CacheReaderPacing, UnpacedTransportSendsTheWholeBatchAtOnce) {
  MockBusLink link;  // interval 0
  ASSERT_TRUE(link.connect());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  for (std::uint16_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A10 + i), {0x00, 0x80, 0x01}));
  }
  auto next_send_at = Clock::time_point{};
  const auto drained = detail::drain_tx_queue(link, queue, {}, 8, 0, next_send_at);
  EXPECT_EQ(drained.sent, 5);
  EXPECT_EQ(drained.wait_ms, 0);
  EXPECT_EQ(queue.size(), 0U);
}

TEST(CacheReaderPacing, BatchSizeBoundsOneDrainCall) {
  MockBusLink link;
  ASSERT_TRUE(link.connect());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  for (std::uint16_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A20 + i), {0x00, 0x80, 0x01}));
  }
  auto next_send_at = Clock::time_point{};
  const auto drained = detail::drain_tx_queue(link, queue, {}, 2, 0, next_send_at);
  EXPECT_EQ(drained.sent, 2);
  EXPECT_EQ(queue.size(), 3U);
}

TEST(CacheReaderPacing, FailedSendLeavesTheTelegramQueued) {
  MockBusLink link;
  ASSERT_TRUE(link.connect());
  link.set_send_fail_count(1);
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A03, {0x00, 0x80, 0x01}));

  auto next_send_at = Clock::time_point{};
  const auto drained = detail::drain_tx_queue(link, queue, {}, 8, 0, next_send_at);
  EXPECT_EQ(drained.sent, 0);
  EXPECT_TRUE(drained.failed);
  EXPECT_EQ(queue.size(), 1U) << "a telegram that could not be sent must stay queued";
}

/// The loop must service reception while it waits for the pacing interval: a
/// paced backlog may not turn into a busy loop either.
TEST(CacheReaderPacing, PacedBacklogDoesNotBusyLoop) {
  MockBusLink link;
  link.set_min_send_interval_ms(20);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  for (std::uint16_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A30 + i), {0x00, 0x80, 0x01}));
  }

  CacheReaderOptions options = FastOptions();
  options.wait_timeout_ms = 100;  // millisecond resolution is enough here
  const auto start = Clock::now();
  run_cache_reader(link, cache, &queue, options, [start] { return ElapsedMs(start) > 150; });

  EXPECT_EQ(link.sent_packets().size(), 4U) << "the backlog must be drained";
  EXPECT_EQ(queue.size(), 0U);
  // 150 ms / 20 ms pacing = ~7 loop iterations; a spin would produce hundreds.
  EXPECT_LT(link.wait_calls(), 100) << "the loop spun instead of waiting";
}

// ===========================================================================
// Global send rate limit: the cache reader is the only sender, so this limit
// applies to every client and every worker at once.
// ===========================================================================

TEST(CacheReaderRateLimit, BoundsTheThroughputOfEveryTelegram) {
  MockBusLink link;  // the transport itself imposes no interval
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.push(static_cast<std::uint16_t>(0x0A40 + i), {0x00, 0x80, 0x01}));
  }

  CacheReaderOptions options = FastOptions();
  options.max_tx_per_sec = 5;  // one telegram per 200 ms
  const auto start = Clock::now();
  run_cache_reader(link, cache, &queue, options, [start] { return ElapsedMs(start) > 150; });

  EXPECT_EQ(link.sent_packets().size(), 1U) << "5/s must not put 3 telegrams on the bus in 150 ms";
  EXPECT_EQ(queue.size(), 2U) << "the rate-limited telegrams stay queued, never dropped";
}

TEST(CacheReaderRateLimit, ZeroMeansNoAdditionalLimit) {
  MockBusLink link;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  SharedTxQueue queue;
  ASSERT_TRUE(queue.create());
  ASSERT_TRUE(queue.push(0x0A50, {0x00, 0x80, 0x01}));
  ASSERT_TRUE(queue.push(0x0A51, {0x00, 0x80, 0x02}));

  CacheReaderOptions options = FastOptions();
  options.max_tx_per_sec = 0;
  const auto start = Clock::now();
  run_cache_reader(link, cache, &queue, options, [start] { return ElapsedMs(start) > 50; });

  EXPECT_EQ(link.sent_packets().size(), 2U);
  EXPECT_EQ(queue.size(), 0U);
}

}  // namespace cvknx
