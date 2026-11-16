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
 * @file bus_link.h
 * @brief Transport seam: one interface for knxd, KNXnet/IP routing and
 *        KNXnet/IP tunnelling.
 *
 * Only the cache reader process owns a real bus link for receiving; every
 * worker writes through a shared TX queue (SharedTxBusLink).  Handlers,
 * router and cache see nothing but this interface — no `if (transport == ...)`
 * may appear outside main.cpp / BusFactory.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cvknx {

/**
 * @brief A connection to the KNX bus that can send and receive group telegrams.
 *
 * Lifecycle: construct → connect() → (poll / send) → disconnect().
 */
class BusLink {
public:
  BusLink() = default;
  virtual ~BusLink() = default;
  BusLink(const BusLink&) = delete;
  BusLink& operator=(const BusLink&) = delete;
  BusLink(BusLink&&) = delete;
  BusLink& operator=(BusLink&&) = delete;

  /// Establish a fully usable connection (including any transport handshake).
  /// Failure must never block the process: implementations use deadlines.
  [[nodiscard]] virtual bool connect() = 0;

  /// Tear the connection down.  Must be safe to call when not connected.
  virtual void disconnect() = 0;

  [[nodiscard]] virtual bool is_connected() const = 0;

  /// Send a group telegram (A_GroupValue_Write / A_GroupValue_Read).
  /// @param group_addr 16-bit EIB group address.
  /// @param apdu Encoded APDU bytes including the 2-byte header.
  /// @return false when the telegram could not be handed to the bus.
  [[nodiscard]] virtual bool send_group_packet(uint16_t group_addr,
                                               const std::vector<uint8_t>& apdu) = 0;

  /// Try to receive one group telegram without blocking.
  /// @return true if a telegram was consumed.
  [[nodiscard]] virtual bool poll_group_telegram(uint16_t& out_group_addr,
                                                 std::vector<uint8_t>& out_apdu) = 0;

  /// Minimum interval between two consecutive send_group_packet() calls, in
  /// milliseconds; 0 = as fast as the transport accepts them.
  ///
  /// The cache reader honours this by *waiting* between sends (the wait is a
  /// poll() timeout, so reception is never blocked).  Transports that would
  /// flood an upstream daemon — knxd's IP tunnel drops telegrams when they
  /// arrive faster than 50 ms apart — override it instead of sleeping inside
  /// send_group_packet(), which would stall the receive path.
  [[nodiscard]] virtual int min_send_interval_ms() const { return 0; }

  /// poll()-able fd, or -1 when the transport has none.
  [[nodiscard]] virtual int get_fd() const = 0;

  /// Total group telegrams received — used for diagnostics.
  [[nodiscard]] virtual uint64_t get_telegram_count() const = 0;

  /// Switch the underlying socket between blocking and non-blocking mode.
  virtual void set_nonblocking(bool enable) = 0;

  /// Result of wait_for_activity().
  enum class WaitResult : std::uint8_t {
    Timeout = 0,    ///< No data within the timeout — the connection is fine.
    GroupData = 1,  ///< Data available.
    Error = 2,      ///< The connection is gone and must be re-established.
  };

  /// Block until data arrives or the timeout expires.  Zero CPU (poll()).
  [[nodiscard]] virtual WaitResult wait_for_activity(int timeout_ms) = 0;

  /// Periodic bookkeeping that has to happen even when the caller polls the
  /// raw fds itself (the cache reader polls the link fd and the TX eventfd
  /// together).  Must be called at least once per second while connected.
  ///
  /// Only transports with keep-alive traffic need this: KNXnet/IP tunnelling
  /// sends its CONNECTIONSTATE_REQUEST here and ends a tunnel whose heartbeat
  /// is not answered.  The default is a no-op.
  virtual void maintain() {}

  /// Human readable target for log lines, e.g. "knxd at /run/knx".
  [[nodiscard]] virtual std::string label() const = 0;

  /// True when the transport has no connection state at all (KNXnet/IP
  /// routing: fire-and-forget multicast).  A successful connect() then proves
  /// nothing about the bus, and a startup probe must not claim otherwise.
  [[nodiscard]] virtual bool is_stateless() const { return false; }
};

}  // namespace cvknx
