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
 * @file bus_status_json.h
 * @brief Report a bus outage to the client in /r and /w responses.
 *
 * The cache keeps serving the last known values while the bus connection is
 * unreachable, so /r looks perfectly healthy — the client would never notice
 * that the values stopped updating.  Whenever the cache reader has no bus
 * connection, /r and /w add an additional object to the response:
 *
 * @code
 *   {"d":{...},"i":42,"knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
 *   {"d":{...},"i":42,"bus":{"ok":false,"since":1763000000,"lastOk":1762990000}}
 * @endcode
 *
 * The key is `"knxd"` for the knxd transport (unchanged for existing
 * deployments) and `"bus"` for the KNXnet/IP transports; it comes from
 * BusConfig::outage_key().  The inner shape is identical.
 *
 * - `ok` is always `false` in this block (its presence already means "not
 *   available"; the field makes that explicit for the client).
 * - `since` is the Unix time (seconds) at which the current outage started.
 * - `lastOk` is the Unix time (seconds) of the last successful connection.
 * Both timestamps are omitted if unknown (never connected / not yet recorded).
 */

#pragma once

#include <string_view>

#include "state/shared_group_cache.h"

namespace cvknx {

class JsonBuilder;

/// Append `"<key>":{"ok":false,...}` when the bus link is unavailable.
///
/// @param json              Builder positioned inside the response object.
/// @param status            State published by the cache reader.
/// @param key               `"knxd"` or `"bus"` (BusConfig::outage_key()).
/// @param force_unavailable Report the outage even if @p status still says the
///                          connection is up (used by /w after a failed send,
///                          where the write itself proves the link is gone).
/// @return true if the block was appended.
bool add_bus_outage(JsonBuilder& json, const SharedGroupCache::BusStatus& status,
                    std::string_view key, bool force_unavailable = false);

}  // namespace cvknx
