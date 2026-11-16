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
 * @file cache_reader.h
 * @brief The bus -> SharedGroupCache pump (one dedicated process).
 *
 * The cache reader owns the only bus link (any transport) for RX and TX.
 * Its loop must be resilient: the bus can be absent at startup or restart at
 * any time.  The loop therefore reconnects forever with an exponential
 * backoff and never spins — a busy loop would burn a full CPU core, which is
 * unacceptable on an embedded system.
 *
 * Extracted from main.cpp so that the reconnect/backoff behaviour is unit
 * testable with MockBusLink.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace cvknx {

class BusLink;
class SharedGroupCache;
class SharedTxQueue;

/// Tunables for run_cache_reader().  Defaults are production values; tests
/// use very small values to keep the runtime short.
struct CacheReaderOptions {
  /// poll() slice while connected.  Also the maximum latency of the stop check.
  int wait_timeout_ms = 1000;
  /// First reconnect delay after a failure.
  int reconnect_backoff_ms = 500;
  /// Upper bound for the reconnect delay (backoff doubles up to this value).
  int max_reconnect_backoff_ms = 30000;
  /// Granularity of interruptible sleeps between reconnect attempts.
  int sleep_slice_ms = 100;
  /// Minimum interval between two "cannot connect / lost connection" warnings.
  /// knxd may be gone for days; the log must not be flooded meanwhile.
  int retry_log_interval_ms = 30000;
  /// Hard upper bound on outgoing telegrams per second — across *all* clients
  /// and *all* workers, because the cache reader is the only process that
  /// sends.  A buggy or hostile client can queue as much as the TX ring
  /// accepts; this limit is what keeps the bus itself from being flooded.
  /// 0 disables the extra limit (tests); production uses 10/s
  /// (`KNX_TX_MAX_PER_SEC`).
  int max_tx_per_sec = 10;
};

/// Pump group telegrams from the bus into the shared cache until should_stop()
/// returns true.
///
/// Guarantees:
///   - never busy-loops, even when the bus link is missing or dies;
///   - retries the connection forever with exponential backoff;
///   - resumes pushing telegrams as soon as the link is usable again;
///   - paces outgoing telegrams by `BusLink::min_send_interval_ms()` without
///     blocking reception: the pacing delay is a poll() timeout, so an inbound
///     telegram is still delivered immediately.
///
/// @param link Bus link to use (must not be connected yet; the loop owns it).
/// @param cache Shared cache that receives the telegrams.
/// @param tx_queue Optional shared TX ring: when given, the loop also drains
///        outgoing telegrams and is woken by its eventfd (zero added latency).
/// @param options Timing configuration.
/// @param should_stop Cooperative stop predicate, evaluated frequently.
void run_cache_reader(BusLink& link, SharedGroupCache& cache, SharedTxQueue* tx_queue,
                      const CacheReaderOptions& options, const std::function<bool()>& should_stop);

/// Building blocks of the loop, exposed so that each wake-up path can be tested
/// on its own (the loop itself only exercises them indirectly).
namespace detail {

/// What woke the cache reader loop.
enum class Activity : std::uint8_t { Timeout, Data, Tx, Error };

/// Outcome of one TX drain batch.
struct TxDrain {
  int sent = 0;
  /// A send failed: the link is in trouble.  The caller must not spin — the
  /// telegram stays queued and is retried after a normal wait.
  bool failed = false;
  /// The transport's minimum send interval has not elapsed yet.  The caller
  /// must not send before this many milliseconds have passed; it should use
  /// the value as its poll() timeout so reception keeps running meanwhile.
  int wait_ms = 0;
};

/// Send waiting telegrams into the bus, honouring @p interval_ms.
///
/// The pacing is expressed as a *wait* (`TxDrain::wait_ms`), never as a sleep
/// inside the link: an inbound telegram must still be delivered immediately
/// (AGENTS.md rule 2).
///
/// @param should_stop Checked between telegrams, so shutdown stays prompt.
///        An empty predicate means "never stop".
/// @param max_batch Upper bound of telegrams per call.
/// @param interval_ms Minimum interval between two consecutive sends, already
///        combined from the transport limit and the global rate limit
///        (`CacheReaderOptions::max_tx_per_sec`); 0 = send as fast as accepted.
/// @param next_send_at Earliest time the next telegram may go out (loop state,
///        updated here).
[[nodiscard]] TxDrain drain_tx_queue(BusLink& link, SharedTxQueue& queue,
                                     const std::function<bool()>& should_stop, int max_batch,
                                     int interval_ms,
                                     std::chrono::steady_clock::time_point& next_send_at);

/// Sleep for @p duration_ms, but wake up early when should_stop() becomes true.
/// Sleeps in slices so a shutdown never waits for a full backoff period.
/// An empty should_stop means "never stop early".
void sleep_interruptible(int duration_ms, int slice_ms, const std::function<bool()>& should_stop);

/// Evaluate a cooperative stop predicate.  An empty predicate means "never
/// stop", which keeps the loop running until the process is killed.
[[nodiscard]] bool stop_requested(const std::function<bool()>& should_stop);

/// Wait for bus traffic, an outgoing telegram, or the timeout — one poll().
/// Never busy-loops: the eventfd is consumed before waiting.
/// @param tx_queue Optional shared TX ring; when null (or without an eventfd)
///        only the bus link is polled.
Activity wait_for_activity(BusLink& link, SharedTxQueue* tx_queue, int timeout_ms);

}  // namespace detail

}  // namespace cvknx
