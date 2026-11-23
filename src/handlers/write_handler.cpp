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
 * @file write_handler.cpp
 * @brief Implementation of the CometVisu /w (write) endpoint handler.
 *
 * Mirrors the reference eibwrite-cgi.c: hex-decodes the `v` parameter,
 * validates the APCI byte, builds an APDU, and sends it via send_group_packet().
 *
 * Extensions beyond eibwrite-cgi:
 *   - Multi-address writes (the `a` parameter can appear multiple times)
 *   - Error reporting: returns HTTP 503 when all writes to valid addresses fail
 *   - Returns 404 when ALL addresses have an invalid format
 */

#include "write_handler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "bus/bus_link.h"
#include "bus_status_json.h"
#include "knxd/knxd_protocol.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknx {

namespace {

/// Largest APDU a write may produce.  A standard cEMI frame carries at most
/// 14 APDU octets (TPCI/APCI + data) and the shared TX ring is sized for
/// exactly that; anything longer would be rejected by the ring *after* the
/// handler accepted it (a 503 for a value the protocol cannot carry).
constexpr std::size_t kMaxWriteApduBytes = 14;

/// Build the APDU for a write from the `v` parameter.
/// @return the APDU, or std::nullopt when the value is not a valid
///         A_GroupValue_Write (bad hex, missing APCI bit, too long).
std::optional<std::vector<uint8_t>> build_write_apdu(std::string_view value) {
  const auto hex_data = hex_decode(value);
  if (hex_data.empty() || (hex_data.front() & 0x80) != 0x80) {
    // The reference eibwrite-cgi expects the APCI byte in `v` ("800c6f" for a
    // 2-byte write of 0x0c6f).  Missing it is a client error, not a success.
    return std::nullopt;
  }
  std::vector<uint8_t> apdu;
  apdu.reserve(1 + hex_data.size());
  apdu.push_back(0x00);
  apdu.insert(apdu.end(), hex_data.begin(), hex_data.end());
  if (apdu.size() > kMaxWriteApduBytes) {
    return std::nullopt;
  }
  return apdu;
}

}  // namespace

WriteHandler::WriteHandler(BusLink& knxd, SessionStore& sessions, const SharedGroupCache* cache,
                           std::string outage_key)
    : knxd_(knxd), sessions_(sessions), cache_(cache), outage_key_(std::move(outage_key)) {}

WriteResult WriteHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};
  WriteResult result;

  // A truncated address list would write to only some of the requested
  // addresses — never do that silently, report the limit violation instead.
  if (params.limit_exceeded()) {
    result.http_status = 400;
    result.body = R"({"error":"too many addresses"})";
    return result;
  }

  // ---- Session: the protocol requires `s` on every request ----
  // A request without a session id is not a write: answer 401 instead of
  // toggling the bus for a client that never logged in.  The anonymous
  // session "0" (returned by /l when no credentials are sent) is valid.
  const auto session = params.get("s");
  if (!session) {
    result.http_status = 401;
    result.body = R"({"error":"missing session"})";
    return result;
  }
  if (!sessions_.is_valid(*session)) {
    result.http_status = 401;
    result.body = R"({"error":"invalid session"})";
    return result;
  }

  // ---- Parameter extraction ----
  const auto addresses = params.get_all("a");
  const auto value_opt = params.get("v");

  // Nothing to write: a request without address or value is a no-op (the
  // reference implementation answers 200 as well).
  if (addresses.empty() || !value_opt) {
    return result;
  }

  // A value that is not a valid A_GroupValue_Write is a client error.  It used
  // to be answered with 200 and an empty body, which made a rejected write look
  // like a successful one.
  const auto apdu = build_write_apdu(*value_opt);
  if (!apdu) {
    result.http_status = 400;
    result.body = R"({"error":"invalid value"})";
    return result;
  }

  // ---- Write to every resolvable address ----
  std::size_t valid = 0;
  std::size_t failed = 0;
  for (const auto& addr_str : addresses) {
    const auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (!parsed) {
      continue;
    }
    ++valid;
    if (!knxd_.send_group_packet(parsed->group.to_eibaddr(), *apdu)) {
      ++failed;
    }
  }

  // 404 only when all addresses had an invalid format.  An empty address list
  // returned early above, so valid == 0 means every entry failed to parse.
  if (valid == 0) {
    result.http_status = 404;
    result.body = R"({"error":"invalid address format"})";
    return result;
  }

  // A failed send must never look like a success.  The response also carries
  // the bus availability block (down since / last successful connection) so
  // the client can show why the write failed.
  if (failed > 0) {
    const SharedGroupCache::BusStatus status =
        cache_ != nullptr ? cache_->bus_status() : SharedGroupCache::BusStatus{};

    JsonBuilder json;
    json.start_object();
    json.add_string("error", failed == valid ? "write failed" : "write partially failed");
    json.add_number("failed", static_cast<int64_t>(failed));
    json.add_number("total", static_cast<int64_t>(valid));
    add_bus_outage(json, status, outage_key_, /*force_unavailable=*/true);
    json.end_object();

    result.http_status = 503;
    result.body = json.take();
  }

  return result;
}

}  // namespace cvknx
