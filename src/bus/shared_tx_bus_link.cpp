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
 * @file shared_tx_bus_link.cpp
 * @brief Worker-side BusLink implementation.
 */

#include "bus/shared_tx_bus_link.h"

namespace cvknx {

SharedTxBusLink::SharedTxBusLink(SharedTxQueue& queue, const SharedGroupCache& cache)
    : queue_(queue), cache_(cache) {}

bool SharedTxBusLink::send_group_packet(std::uint16_t group_addr,
                                        const std::vector<std::uint8_t>& apdu) {
  if (!queue_.is_initialized()) {
    return false;
  }
  // While the reader has no bus connection, enqueueing would pretend success:
  // the write would sit in the queue and the client would get a 200 for a
  // telegram that never reached the bus.  Reject instead, so /w reports 503.
  if (!cache_.bus_status().available) {
    return false;
  }
  if (!queue_.push(group_addr, apdu)) {
    return false;  // full or payload too long
  }
  queue_.signal();
  return true;
}

bool SharedTxBusLink::poll_group_telegram(std::uint16_t& out_group_addr,
                                          std::vector<std::uint8_t>& out_apdu) {
  // Workers never receive telegrams: the cache reader owns the bus connection
  // for RX as well and pushes inbound values straight into the shared cache.
  // A worker that could read here would steal telegrams from the other
  // workers' long-polls, so the method stays a no-op.
  (void)out_group_addr;
  (void)out_apdu;
  return false;
}

}  // namespace cvknx
