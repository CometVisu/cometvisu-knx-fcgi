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
 * @file write_handler.h
 * @brief CometVisu write endpoint handler — sends a group telegram to the bus.
 *
 * Equivalent to the reference eibwrite-cgi.c: parses the address and hex
 * value from the query string, builds an APDU, and hands it to the shared TX
 * ring (the cache reader is the only process that talks to the bus).
 *
 * Key differences from eibwrite-cgi:
 *   - Multi-address writes: the `a` parameter can appear multiple times.
 *   - Strict input validation: a value that is not an A_GroupValue_Write
 *     answers 400 instead of pretending success.
 *   - Error reporting: answers 503 when a write could not be handed to the bus
 *     (with the number of failed addresses), instead of ignoring the failure.
 *   - Write pacing is applied by the cache reader (`BusLink::min_send_interval_ms()`),
 *     not by this handler: blocking here would stall the process that also
 *     receives the telegrams.
 */

#pragma once

#include <string>
#include <string_view>

namespace cvknx {

class BusLink;
class SessionStore;
class SharedGroupCache;

/**
 * @brief Result of a write operation (POST /w).
 */
struct WriteResult {
  /// HTTP status code (200, 400, 401, 404, 503).
  int http_status = 200;
  /// Response body: empty on success, JSON error details on failure.
  std::string body;
};

/**
 * @brief Handles CometVisu write requests: `POST /w?a=ADDRESS&v=HEXVALUE&s=SESSION`
 *
 * The `v` parameter contains the hex-encoded APDU value with the APCI byte
 * already set (e.g. `v=8042` for a 1-byte write of 0x42).  This matches the
 * eibwrite-cgi convention.
 */
class WriteHandler {
public:
  /// @param knxd   Bus link used to send the group telegram.
  /// @param sessions Session store for `s=` validation.
  /// @param cache  Optional shared cache; when given, a failed write also
  ///               reports since when the bus has been unavailable.
  /// @param outage_key JSON key of the outage block (`"knxd"` or `"bus"`).
  WriteHandler(BusLink& knxd, SessionStore& sessions, const SharedGroupCache* cache = nullptr,
               std::string outage_key = "knxd");

  ~WriteHandler() = default;

  // Reference members prevent copy/move.
  WriteHandler(const WriteHandler&) = delete;
  WriteHandler& operator=(const WriteHandler&) = delete;
  WriteHandler(WriteHandler&&) = delete;
  WriteHandler& operator=(WriteHandler&&) = delete;

  /// @brief Process a write request.
  /// @param query_string Raw QUERY_STRING from FCGI (e.g. "a=1/2/3&v=8042").
  /// @return WriteResult with HTTP status code (200, 400, 401, 404, 503).
  [[nodiscard]] WriteResult handle(std::string_view query_string);

private:
  BusLink& knxd_;           // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  SessionStore& sessions_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  const SharedGroupCache* cache_ = nullptr;
  std::string outage_key_;
};

}  // namespace cvknx
