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
 * @file knxd_client.h
 * @brief Single-connection knxd Unix socket client.
 *
 * Opens one group socket to knxd for sending group telegrams
 * (EIB_GROUP_PACKET) and receiving APDU_PACKET telegrams from the
 * KNX bus.  The cache reader is the only user; it owns the connection and is
 * also the only place that reconnects.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bus/bus_link.h"

namespace cvknx {

/// knxd transport — POSIX Unix socket speaking the eibd client protocol.
class KnxdClient : public BusLink {
public:
  /// @param socket_path knxd's Unix socket (default: /run/knx).
  explicit KnxdClient(std::string socket_path = "/run/knx");
  ~KnxdClient() override;

  // Move-only (owns socket fd).
  KnxdClient(KnxdClient&&) noexcept;
  KnxdClient& operator=(KnxdClient&&) noexcept;
  KnxdClient(const KnxdClient&) = delete;
  KnxdClient& operator=(const KnxdClient&) = delete;

  /// Connect to the configured socket and open a read-write group socket.
  [[nodiscard]] bool connect() override;

  /// Connect to an explicit socket path (tests, transitional call sites).
  /// Opens the socket only — the group socket handshake is separate.
  [[nodiscard]] bool connect(std::string_view socket_path);

  void disconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool open_group_socket(bool write_only);
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;

  /// knxd's IP tunnel drops telegrams that follow each other faster than
  /// ~50 ms.  The cache reader applies the interval by waiting between sends;
  /// this class never sleeps in the receive path.
  [[nodiscard]] int min_send_interval_ms() const override { return 50; }
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;

  /// @brief Maximum time to wait for the EIB_OPEN_GROUPCON reply.
  ///
  /// Without a deadline, a knxd that accepts the connection but never answers
  /// blocks the caller forever (this used to hang /w requests).
  void set_handshake_timeout_ms(int timeout_ms);

  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;

  [[nodiscard]] std::string label() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvknx
