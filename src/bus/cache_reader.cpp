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
 * @file cache_reader.cpp
 * @brief Implementation of the resilient bus -> SharedGroupCache pump.
 */

#include "bus/cache_reader.h"

#include <poll.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "bus/bus_link.h"
#include "bus/knx_codec.h"
#include "state/shared_group_cache.h"
#include "state/shared_tx_queue.h"
#include "util/debug_log.h"
#include "util/hex.h"

namespace cvknx {

namespace detail {

void sleep_interruptible(int duration_ms, int slice_ms, const std::function<bool()>& should_stop) {
  const int slice = std::max(1, slice_ms);
  int remaining = duration_ms;
  while (remaining > 0) {
    if (stop_requested(should_stop)) {
      return;
    }
    const int chunk = std::min(remaining, slice);
    struct timespec request {};
    request.tv_sec = chunk / 1000;
    request.tv_nsec = static_cast<long>(chunk % 1000) * 1000000L;
    // EINTR (signal) is expected — the loop re-checks the stop flag.
    (void)::nanosleep(&request, nullptr);
    remaining -= chunk;
  }
}

bool stop_requested(const std::function<bool()>& should_stop) {
  return static_cast<bool>(should_stop) && should_stop();
}

}  // namespace detail

namespace {

/// Push every pending group telegram into the shared cache.
/// @return number of telegrams that were pushed.
int drain_telegrams(BusLink& link, SharedGroupCache& cache) {
  int pushed = 0;
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (link.poll_group_telegram(addr, apdu)) {
    // Only the three group-value services are interesting; a management
    // frame, a reserved APCI or a read request is dropped here.
    const auto parsed = parse_apdu(apdu);
    if (!parsed || parsed->type == ApduType::Read) {
      continue;
    }
    cache.push(addr, parsed->data);
    DebugLog::knxd_recv("cache_push", KnxGroupAddress::from_eibaddr(addr).to_string(),
                        hex_encode(parsed->data.data(), parsed->data.size()));
    ++pushed;
  }
  return pushed;
}

}  // namespace

namespace detail {

/// Maximum telegrams sent per drain call.
///
/// A rate-limited bus (a slow tunnel, a congested knxd) would otherwise let a
/// burst of writes block the loop for seconds: no inbound telegram would be
/// read in that time and a SIGTERM would only be noticed after the whole
/// backlog.  Eight telegrams per batch keeps both latencies bounded.
inline constexpr int kMaxTxBatch = 8;

/// Send waiting telegrams into the bus.
///
/// Clears the eventfd first, so a push during draining re-arms it.  Telegrams
/// are paced by @p interval_ms; the pacing is expressed as a wait *after* the
/// batch, never as a sleep inside the loop, so an inbound telegram is still
/// delivered immediately (AGENTS.md rule 2).
///
/// @param stop_requested Checked between telegrams, so shutdown stays prompt.
/// @param next_send_at Earliest time the next telegram may go out (state of
///        the loop, updated here).
TxDrain drain_tx_queue(BusLink& link, SharedTxQueue& queue,
                       const std::function<bool()>& should_stop, int max_batch, int interval_ms,
                       std::chrono::steady_clock::time_point& next_send_at) {
  queue.drain_notifications();
  TxDrain result;
  const auto interval = std::chrono::milliseconds(std::max(0, interval_ms));
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  // An empty predicate means "never stop" (see stop_requested()).
  while (result.sent < max_batch && !stop_requested(should_stop) && queue.peek(addr, apdu)) {
    if (interval.count() > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_send_at) {
        result.wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(next_send_at - now).count() + 1);
        break;
      }
    }
    if (!link.send_group_packet(addr, apdu)) {
      // The link is gone.  The telegram stays queued and is sent after the
      // reconnect — a write must never be lost silently.
      DebugLog::log(DebugLog::Level::Warn, "failed to send queued telegram to " + link.label());
      result.failed = true;
      break;
    }
    queue.pop_drop();
    ++result.sent;
    next_send_at = std::chrono::steady_clock::now() + interval;
    DebugLog::knxd_send("tx_queue", KnxGroupAddress::from_eibaddr(addr).to_string(),
                        hex_encode(apdu.data(), apdu.size()));
  }
  return result;
}

/// Wait for bus traffic, an outgoing telegram, or the timeout — one poll().
/// Never busy-loops: the eventfd is consumed by drain_tx_queue().
Activity wait_for_activity(BusLink& link, SharedTxQueue* tx_queue, int timeout_ms) {
  // A link can die while it is not being polled (a device dropped the tunnel,
  // a router signalled an error): every iteration must notice that before
  // blocking again, otherwise the reconnect never happens.
  if (!link.is_connected()) {
    return Activity::Error;
  }
  if (tx_queue == nullptr || tx_queue->eventfd() < 0) {
    const auto result = link.wait_for_activity(timeout_ms);
    if (result == BusLink::WaitResult::GroupData) {
      return Activity::Data;
    }
    if (result == BusLink::WaitResult::Timeout) {
      return Activity::Timeout;
    }
    return Activity::Error;
  }

  struct pollfd fds[2]{};
  nfds_t count = 0;
  int link_index = -1;
  const int link_fd = link.get_fd();
  if (link_fd >= 0) {
    fds[count].fd = link_fd;
    fds[count].events = POLLIN;
    link_index = static_cast<int>(count);
    ++count;
  }
  const int tx_index = static_cast<int>(count);
  fds[count].fd = tx_queue->eventfd();
  fds[count].events = POLLIN;
  ++count;

  const int rc = ::poll(fds, count, timeout_ms);
  if (rc <= 0) {
    return Activity::Timeout;  // timeout or EINTR — the caller re-checks stop
  }
  if ((fds[tx_index].revents & POLLIN) != 0) {
    return Activity::Tx;
  }
  // Without a link fd only the eventfd was polled — and that wake-up returned
  // above, so the false arm cannot be reached.
  if (link_index >= 0) {  // GCOV_EXCL_BR_LINE
    if ((fds[link_index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return Activity::Error;
    }
    // poll() reports POLLIN for any readable socket, so the false arm (a
    // readable link fd without POLLIN or an error flag) cannot occur.
    if ((fds[link_index].revents & POLLIN) != 0) {  // GCOV_EXCL_BR_LINE
      return Activity::Data;
    }
  }
  return Activity::Timeout;  // GCOV_EXCL_LINE - every revents combination returned above
}

}  // namespace detail

void run_cache_reader(BusLink& link, SharedGroupCache& cache, SharedTxQueue* tx_queue,
                      const CacheReaderOptions& options, const std::function<bool()>& should_stop) {
  using detail::Activity;
  using detail::drain_tx_queue;
  using detail::sleep_interruptible;
  using detail::stop_requested;
  using detail::wait_for_activity;

  const int initial_backoff_ms = std::max(1, options.reconnect_backoff_ms);
  const int max_backoff_ms = std::max(initial_backoff_ms, options.max_reconnect_backoff_ms);
  const int wait_timeout_ms = std::max(1, options.wait_timeout_ms);

  // Global send pacing.  Exactly one process (this loop) talks to the bus, so
  // an interval here limits *every* client and *every* worker at once: a
  // client that asks for thousands of writes — or a buggy one that retries in
  // a loop — can never flood the bus.  The transport's own minimum interval
  // is the floor (knxd's IP tunnel drops telegrams sent closer than 50 ms
  // apart), the global rate limit is the ceiling.
  // 10/s -> 100 ms, 3/s -> 334 ms (ceil, never faster than requested).
  const int rate_interval_ms =
      options.max_tx_per_sec > 0 ? (1000 + options.max_tx_per_sec - 1) / options.max_tx_per_sec : 0;
  const int tx_interval_ms = std::max(link.min_send_interval_ms(), rate_interval_ms);

  int backoff_ms = initial_backoff_ms;
  bool connected = false;

  bool logged_failure = false;
  std::chrono::steady_clock::time_point last_log{};

  // Passing an empty std::function is allowed and means "never stop".
  const auto stop = [&should_stop]() { return stop_requested(should_stop); };

  // Rate-limited warning while the bus is unreachable.
  const int retry_log_interval_ms = std::max(0, options.retry_log_interval_ms);
  const auto log_retry = [&logged_failure, &last_log,
                          retry_log_interval_ms](const std::string& reason) {
    const auto now = std::chrono::steady_clock::now();
    const auto since_last_log =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
    const bool due = !logged_failure || since_last_log >= retry_log_interval_ms;
    if (due) {
      DebugLog::log(DebugLog::Level::Warn, reason + " — retrying in the background");
      last_log = now;
      logged_failure = true;
    }
  };

  // Earliest time the next telegram may be handed to the bus (transport
  // specific pacing; zero for transports without a rate limit).
  auto next_send_at = std::chrono::steady_clock::time_point{};

  // Start out unavailable: until the first handshake succeeds there is no
  // bus connection, and /r clients should see that right away.
  cache.publish_bus_status(false);

  while (!stop()) {
    // ---------------- (Re)connect ----------------
    if (!connected) {
      if (link.connect()) {
        link.set_nonblocking(true);
        connected = true;
        backoff_ms = initial_backoff_ms;
        cache.publish_bus_status(true);
        next_send_at = std::chrono::steady_clock::time_point{};

        if (logged_failure) {
          DebugLog::log(DebugLog::Level::Info, "bus connection restored to " + link.label());
          logged_failure = false;
        }
        continue;  // immediately look for pending telegrams
      }

      // Make sure a half-open connection does not survive the failed attempt.
      link.disconnect();
      cache.publish_bus_status(false);  // keeps the first outage timestamp

      log_retry("cannot connect to " + link.label());
      sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
      backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
      continue;
    }

    // ---------------- Drain pending writes ----------------
    // Always before waiting: a telegram queued while we were busy must not
    // wait for the next poll slice.  Re-loop when something was sent so the
    // stop predicate and the queue are re-evaluated before blocking again.
    int sent = 0;
    bool backlog = false;
    bool send_failed = false;
    int pace_wait_ms = 0;
    if (tx_queue != nullptr) {
      const auto drained =
          drain_tx_queue(link, *tx_queue, stop, detail::kMaxTxBatch, tx_interval_ms, next_send_at);
      sent = drained.sent;
      send_failed = drained.failed;
      pace_wait_ms = drained.wait_ms;
      // A failing link must not turn the backlog into a busy loop: fall through
      // to a normal (sleeping) wait and retry after it.
      backlog = (tx_queue->size() > 0) && !send_failed;
    }
    if (sent > 0 && !backlog) {
      continue;
    }

    // ---------------- Wait for activity ----------------
    // While a backlog remains, poll with a zero timeout: that services inbound
    // telegrams between two write batches instead of starving them.  A pacing
    // delay is a real timeout, so reception keeps running while we wait for
    // the transport's rate limit to expire.
    //
    // maintain() runs before every wait: this loop polls the raw fds, so a
    // transport that needs keep-alive traffic (KNXnet/IP tunnelling) would
    // otherwise never send its heartbeat.
    link.maintain();
    int wait_ms = wait_timeout_ms;
    if (backlog) {
      // Zero while a backlog is being drained; otherwise the remaining pacing
      // delay (which is a poll timeout, so RX keeps working).
      wait_ms = pace_wait_ms > 0 ? pace_wait_ms : 0;
    }
    Activity activity = Activity::Timeout;
    if (!link.is_connected()) {
      activity = Activity::Error;
    } else {
      activity = detail::wait_for_activity(link, tx_queue, wait_ms);
    }
    if (activity == Activity::Tx) {
      continue;  // a worker queued a telegram (or the pacing delay expired)
    }
    if (activity == Activity::Data) {
      if (drain_telegrams(link, cache) > 0) {
        backoff_ms = initial_backoff_ms;
      }
      // The datagram just read may have announced a dead connection.
      if (!link.is_connected()) {
        activity = Activity::Error;
      }
    }
    if (activity == Activity::Error) {
      // Connection lost (daemon restarted / "Link down, terminating" /
      // router gone).  Never fall through to a busy loop: back off, then
      // reconnect.
      DebugLog::log(DebugLog::Level::Warn,
                    "lost connection to " + link.label() + " — reconnecting");
      logged_failure = true;
      last_log = std::chrono::steady_clock::now();

      link.disconnect();
      connected = false;
      cache.publish_bus_status(false);
      sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
      backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
    }
    // Activity::Timeout needs no action — the loop re-checks the stop flag.
  }
}

}  // namespace cvknx
