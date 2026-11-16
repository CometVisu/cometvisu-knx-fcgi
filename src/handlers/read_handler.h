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
 * @file read_handler.h
 * @brief CometVisu read endpoint handler — shared cache and COMET long-poll.
 *
 * Implements the equivalent of the reference eibread-cgi.c's main loop:
 * subscribes to addresses, reads cached values on first request, then
 * blocks waiting for new data via the shared cache's condition variable.
 *
 * The shared cache (SharedGroupCache) is populated by a dedicated cache
 * reader process that owns the single bus connection of the configured
 * transport.  Workers query the cache directly — no worker ever holds a bus
 * connection of its own.  This ensures a single, authoritative position
 * counter (`i`) across all workers, preventing duplicate delivery and
 * non-monotonic index bugs.
 *
 * Key differences from eibread-cgi:
 *   - Uses a shared-memory cache with process-shared mutex/condvar
 *     instead of per-process caches or knxd's built-in cache.
 *   - For t=0 (read from bus): enqueues GroupValueRead telegrams for uncached
 *     addresses into the shared TX ring, then returns immediately.
 *     Read-responses arrive asynchronously and are handled like normal bus
 *     writes by the cache reader process.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cvknx {

/// Hard upper bound for a single request's wait, in seconds.
///
/// The client re-creates a long-poll after 300 s; waiting a little longer
/// (330 s) means the backend's answer wins the race instead of client and
/// backend fighting over every reconnect.  A *session* is not affected by this
/// limit — it lives for minutes (`SESSION_TTL_SEC`).
inline constexpr int kMaxClientWaitSec = 330;

/// Clamp a client supplied wait to kMaxClientWaitSec.
///
/// A value <= 0 is returned unchanged: it means "do not wait at all" (the
/// CometVisu protocol uses `t=0` for "read now" and `t<0` for "cache only").
[[nodiscard]] int clamp_client_wait(int requested_sec);

class BusLink;
class SessionStore;
class SharedGroupCache;

/**
 * @brief Result of a read operation (GET /r).
 */
struct ReadResult {
  /// HTTP status code (200, 400, 401, 404, 503).
  int http_status = 200;
  /// @brief JSON response body (e.g. {"d":{"1/2/3":"42"},"i":141}).
  std::string body;
};

/**
 * @brief Handles CometVisu read requests: `GET /r?s=SESSION&a=ADDRESS&t=TIMEOUT&i=INDEX`
 *
 * This is the most complex handler due to COMET (long-poll) support.
 * The request can block for up to `kMaxClientWaitSec` seconds waiting for a
 * value change, burning zero CPU while blocked on the shared condition
 * variable.
 *
 * Three KNX semantic correctness rules are enforced (see AGENTS.md):
 *   -# No duplicate delivery — values are deduplicated within a response.
 *   -# Immediate delivery — group telegrams wake the handler immediately.
 *   -# Authoritative index — the `i` field always comes from the shared cache.
 */
class ReadHandler {
public:
  /// @param cache Shared cache for group telegrams.
  /// @param knxd  Bus link used to send GroupValueRead on t=0.
  /// @param sessions Session store for `s=` validation.
  /// @param longpoll_timeout_sec Maximum long-poll duration; clamped to
  ///        kMaxClientWaitSec so no request can wait longer.
  /// @param outage_key JSON key of the outage block (`"knxd"` or `"bus"`).
  ReadHandler(SharedGroupCache& cache, BusLink& knxd, SessionStore& sessions,
              int longpoll_timeout_sec = 300, std::string outage_key = "knxd");

  ~ReadHandler() = default;

  // Reference members prevent copy/move.
  ReadHandler(const ReadHandler&) = delete;
  ReadHandler& operator=(const ReadHandler&) = delete;
  ReadHandler(ReadHandler&&) = delete;
  ReadHandler& operator=(ReadHandler&&) = delete;

  /// @brief Process a read request.
  /// @param query_string Raw QUERY_STRING from FCGI (e.g. "a=1/2/3&t=5&i=140").
  /// @return ReadResult with HTTP status code and JSON body.
  [[nodiscard]] ReadResult handle(std::string_view query_string);

private:
  SharedGroupCache& cache_;  // NOLINT — shared cache for group telegrams
  BusLink& knxd_;            // NOLINT — for sending GroupValueRead on t=0
  SessionStore& sessions_;   // NOLINT
  int longpoll_timeout_sec_;
  std::string outage_key_;

  /// Parse a whole-number request parameter (`t` or `i`).
  ///
  /// Both use the same syntax, so one parser serves them; anything that is not
  /// a complete decimal number (including "", "5x", "1.5") yields
  /// std::nullopt and lets the caller decide whether that is a client error
  /// (`t`) or simply "not provided" (`i`).
  [[nodiscard]] static std::optional<int> parse_int_parameter(std::string_view text);
};

}  // namespace cvknx
