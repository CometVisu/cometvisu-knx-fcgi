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
 * @file shared_tx_bus_link.h
 * @brief Worker-side BusLink: enqueue into the shared TX queue, never connect.
 *
 * The cache reader is the single bus owner.  Workers use this link so that
 * `/w` does not need a bus connection of its own — no knxd client slot per
 * worker, no tunnel per worker.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bus/bus_link.h"
#include "state/shared_group_cache.h"
#include "state/shared_tx_queue.h"

namespace cvknx {

class SharedTxBusLink : public BusLink {
public:
  /// @param queue Shared TX ring (created before fork).
  /// @param cache Shared cache — its published status decides whether a write
  ///              is even attempted ("never enqueue during an outage").
  SharedTxBusLink(SharedTxQueue& queue, const SharedGroupCache& cache);

  [[nodiscard]] bool connect() override { return queue_.is_initialized(); }
  void disconnect() override {}
  [[nodiscard]] bool is_connected() const override { return queue_.is_initialized(); }

  /// Enqueue the telegram and wake the cache reader.
  /// @return false when the bus is unavailable or the queue is full — `/w`
  ///         turns that into a 503, exactly like a failed direct write.
  [[nodiscard]] bool send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) override;

  [[nodiscard]] bool poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override { return -1; }
  [[nodiscard]] uint64_t get_telegram_count() const override { return 0; }
  void set_nonblocking(bool enable) override { (void)enable; }
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override {
    (void)timeout_ms;
    return WaitResult::Timeout;  // workers never receive
  }
  [[nodiscard]] std::string label() const override { return "shared TX queue"; }

private:
  SharedTxQueue& queue_;           // NOLINT — shared ring, outlives workers
  const SharedGroupCache& cache_;  // NOLINT — outage state publisher
};

}  // namespace cvknx
