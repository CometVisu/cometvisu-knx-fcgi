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
 * @file read_handler.cpp
 * @brief CometVisu /r endpoint — shared-memory cache based.
 *
 * The shared cache (SharedGroupCache) is populated by a dedicated cache
 * reader process.  This handler queries the cache directly.  Long-poll
 * blocks on the shared condition variable — zero CPU, immediate wake
 * on new data.
 *
 * The `i` value always comes from the shared cache's position counter,
 * which is monotonically increasing across all worker processes.
 */

#include "read_handler.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
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

/// Clamp a client supplied `t` to the hard wait limit.
///
/// `t` is both the age filter and the long-poll duration, so an unclamped
/// value would let one request occupy a worker for years.
int clamp_client_wait(int requested_sec) {
  return requested_sec > kMaxClientWaitSec ? kMaxClientWaitSec : requested_sec;
}

ReadHandler::ReadHandler(SharedGroupCache& cache, BusLink& knxd, SessionStore& sessions,
                         int longpoll_timeout_sec, std::string outage_key)
    : cache_(cache),
      knxd_(knxd),
      sessions_(sessions),
      longpoll_timeout_sec_(std::clamp(longpoll_timeout_sec, 1, kMaxClientWaitSec)),
      outage_key_(std::move(outage_key)) {}

std::optional<int> ReadHandler::parse_int_parameter(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  int val = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  // from_chars stops at the first non-digit; an unconsumed tail means the
  // parameter is not a number (e.g. "12abc").
  if (ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return val;
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};
  ReadResult result;

  // A truncated parameter list would silently drop addresses: the client would
  // never receive values for them (CometVisu issue #1521).  Report it instead.
  if (params.limit_exceeded()) {
    result.http_status = 400;
    result.body = R"({"error":"too many addresses"})";
    return result;
  }

  // ---- Session: the protocol requires `s` on every request ----
  // A request without a session id is not a read: answer 401 instead of
  // serving values to a client that never logged in.  The anonymous session
  // "0" (returned by /l when no credentials are sent) is a valid session id.
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

  // ---- Parse parameters ----
  const auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    result.body = R"({"error":"missing address"})";
    return result;
  }
  // ---- Parse t= parameter (poll timeout AND age filter) ----
  // Per spec (https://github.com/CometVisu/CometVisu/wiki/Protokoll):
  //   t not provided → long-poll, no age filter
  //   t = 0         → read from bus (we approximate: force initial cache read)
  //   t > 0         → cached data ≤ t seconds old, then poll for t seconds
  //   t < 0         → cache only, no bus read (no poll, no age filter)
  int timeout_sec = longpoll_timeout_sec_;
  bool t_provided = false;
  int t_original = 0;
  if (auto t_opt = params.get("t")) {
    const auto parsed = parse_int_parameter(*t_opt);
    if (!parsed) {
      result.http_status = 400;
      result.body = R"({"error":"invalid timeout"})";
      return result;
    }
    t_provided = true;
    t_original = *parsed;
    // No request may wait longer than the hard limit: a client could
    // otherwise pin a worker for years (the value *is* the wait).
    timeout_sec = clamp_client_wait(*parsed);
  }

  // Age filter: only active when t > 0 (spec: "maximal TIMEOUT Sekunden alt").
  int max_age_sec = -1;  // -1 = no age filter
  if (t_provided && t_original > 0) {
    max_age_sec = t_original;
  }

  uint32_t lastpos = 0;
  if (auto i_opt = params.get("i")) {
    const auto parsed = parse_int_parameter(*i_opt);
    if (parsed && *parsed >= 0) {
      lastpos = static_cast<uint32_t>(*parsed);
    }
  }

  // t=0: force initial read (spec: "versucht das Backend die Daten zu lesen").
  // t<0: force initial read + no poll (spec: "nur aus dem Cache gelesen").
  if (t_provided && t_original == 0) {
    lastpos = 0;
    timeout_sec = 1;  // short poll for immediate return after initial read
  } else if (t_provided && t_original < 0) {
    lastpos = 0;
    timeout_sec = 0;  // no poll — return immediately
  }

  // ---- Build EIB address set ----
  std::set<uint16_t> eib_addrs;
  for (const auto& addr_str : addresses) {
    const auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed) {
      eib_addrs.insert(parsed->group.to_eibaddr());
    }
  }
  if (eib_addrs.empty()) {
    result.http_status = 404;
    result.body = R"({"error":"no valid addresses"})";
    return result;
  }

  // GCOV_EXCL_BR_LINE - the branches on the next line come from the inlined
  // std::string member of KnxAddress, not from a source-level condition.
  const auto addr_key = [](uint16_t a) -> std::string {
    KnxAddress address;
    address.group = KnxGroupAddress::from_eibaddr(a);
    return address.to_cometvisu();
  };

  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();
  std::set<uint16_t> already_written;

  // ---- Initial read: latest values from shared cache ----
  // max_age_sec follows the spec:
  //   t not provided → -1 (no age filter, long-poll initial read)
  //   t = 0         → -1 (no age filter, "read from bus")
  //   t > 0         → t  (spec: "maximal TIMEOUT Sekunden alt")
  //   t < 0         → -1 (no age filter, "nur aus dem Cache")
  if (lastpos == 0) {
    // One atomic snapshot: values and position are read under the same lock.
    // Reading the values first and the position afterwards loses a telegram
    // that is pushed in between — the reported `i` would already cover it, so
    // the value could never be delivered (no later delta matches it).
    const auto snapshot = cache_.get_delta(0, eib_addrs, max_age_sec);
    // Iterate the requested addresses (not the returned hash map) so the JSON
    // key order stays deterministic.
    for (auto addr : eib_addrs) {
      const auto it = snapshot.values.find(addr);
      if (it != snapshot.values.end()) {
        json.add_string(addr_key(addr), hex_encode(it->second.data(), it->second.size()));
        already_written.insert(addr);
      }
    }
    lastpos = snapshot.position;
  }

  // ---- t=0: send GroupValueRead for uncached addresses ----
  // Per spec, t=0 means "read from bus NOW" — for any address not in the cache,
  // issue a GroupValueRead telegram.  The response arrives asynchronously and is
  // handled like a normal bus write by the cache reader process.  The response
  // to THIS request is returned immediately (non-blocking).
  if (t_provided && t_original == 0) {
    // Build GroupValueRead APDU: [0x00, 0x00] = A_GroupValue_Read
    const std::vector<uint8_t> read_apdu = {0x00, 0x00};
    for (auto addr : eib_addrs) {
      if (already_written.contains(addr)) {
        continue;
      }
      if (!knxd_.send_group_packet(addr, read_apdu)) {
        // The bus is unreachable or the shared TX ring is full.  Generating
        // more telegrams would only hammer a queue that cannot accept them;
        // these bus reads are best effort (the answer already carries the
        // cached values), so stop here instead of flooding the ring.
        break;
      }
    }
  }

  // ---- Nothing can arrive while knxd is unreachable ----
  // Waiting for the full long-poll timeout and only then reporting the outage
  // would keep the client from ever showing it.  Report right away instead;
  // the response still carries the last known values and the outage block.
  if (timeout_sec > 0 && !cache_.bus_status().available) {
    timeout_sec = 0;
  }

  // ---- Poll loop — query delta, then wait on the shared condition variable ----
  auto tstart = std::chrono::steady_clock::now();
  bool written = !already_written.empty();

  while (timeout_sec > 0) {
    // If the initial read already delivered data, skip the poll loop.
    if (written) {
      break;
    }

    // Query the cache for entries newer than the client's last-known position
    // *before* blocking.  Telegrams that arrived between two requests are
    // already in the cache; waiting for the condition variable first would
    // delay them until the next telegram — on a quiet bus possibly for hours.
    // max_age_sec follows the spec:
    //   t > 0 → entries ≤ t seconds old; t=0/t<0/no-t → no age filter.
    // The position-based filter (pushed_at > lastpos) guarantees we only
    // return new data.  Age filtering is an additional constraint per spec.
    auto delta = cache_.get_delta(lastpos, eib_addrs, max_age_sec);

    // already_written is empty here: as soon as it holds anything the loop is
    // left above, so no duplicate check is needed.
    for (const auto& [addr, val] : delta.values) {
      json.add_string(addr_key(addr), hex_encode(val.data(), val.size()));
      already_written.insert(addr);
      written = true;
    }

    // Position comes from the shared cache — single source of truth,
    // monotonically increasing across all workers.
    lastpos = delta.position;

    if (written) {
      break;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tstart)
            .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0) {
      break;
    }

    // Block on the shared condition variable — zero CPU.
    // Wakes immediately when the cache reader pushes new data.
    bool new_data = cache_.wait_for_new_data(remaining * 1000);
    if (!new_data) {
      break;
    }
  }

  json.end_object();  // d
  json.add_number("i", lastpos);

  // The cache keeps serving the last known values while knxd is unreachable,
  // so without this block the client could not tell "bus is quiet" from
  // "connection lost" — it would show frozen values indefinitely.
  // A read is also not successful while the bus connection is gone, so it
  // answers 503; the body still carries the last known values.
  const auto bus_status = cache_.bus_status();
  add_bus_outage(json, bus_status, outage_key_);

  json.end_object();  // root

  if (!bus_status.available) {
    result.http_status = 503;
  }

  result.body = json.take();
  return result;
}

}  // namespace cvknx