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
 * @file tunneling_bus_link.h
 * @brief KNXnet/IP tunnelling client (one tunnel, owned by the cache reader).
 *
 * Sequence rules (KNXnet/IP, chapter 3/8):
 *   - one counter for all frames sent on the channel, 0..255 then wrap;
 *   - every received TUNNELLING_REQUEST is ACKed with its own sequence number;
 *   - a frame whose sequence number is not the expected one is ACKed and
 *     discarded — delivering it twice would toggle a switch twice (AGENTS.md
 *     rule 1);
 *   - a missing ACK retransmits the *same* sequence number up to `retries`
 *     times, then the telegram is dropped and logged;
 *   - L_Data.con (our own write confirmation) never reaches the cache.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "bus/bus_config.h"
#include "bus/bus_link.h"

namespace cvknx {

struct TunnelOptions {
  std::string host;
  std::uint16_t port = 3671;
  /// Local address advertised in the HPAI (empty = 0.0.0.0, let the device pick).
  std::string local_addr;
  /// Local bind port (0 = ephemeral).
  std::uint16_t local_port = 0;
  bool nat = false;
  int heartbeat_sec = 60;
  int ack_timeout_ms = 1000;
  int retries = 3;
  /// Deadline for the CONNECT_RESPONSE.
  int connect_timeout_ms = 5000;

  [[nodiscard]] static TunnelOptions from_config(const BusConfig& config);
};

class TunnelingBusLink : public BusLink {
public:
  explicit TunnelingBusLink(const TunnelOptions& options);
  ~TunnelingBusLink() override;

  TunnelingBusLink(const TunnelingBusLink&) = delete;
  TunnelingBusLink& operator=(const TunnelingBusLink&) = delete;

  [[nodiscard]] bool connect() override;
  void disconnect() override;
  [[nodiscard]] bool is_connected() const override { return connected_ && !lost_; }

  [[nodiscard]] bool send_group_packet(std::uint16_t group_addr,
                                       const std::vector<std::uint8_t>& apdu) override;
  [[nodiscard]] bool poll_group_telegram(std::uint16_t& out_group_addr,
                                         std::vector<std::uint8_t>& out_apdu) override;

  [[nodiscard]] int get_fd() const override { return fd_; }
  [[nodiscard]] std::uint64_t get_telegram_count() const override { return telegram_count_; }
  void set_nonblocking(bool enable) override;
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;
  void maintain() override;
  [[nodiscard]] std::string label() const override;

  // ---- diagnostics / tests ----
  [[nodiscard]] std::uint8_t channel() const { return channel_; }
  [[nodiscard]] std::uint8_t next_sequence() const { return tx_sequence_; }

private:
  /// Read and handle one datagram.  @return false when nothing could be read.
  bool receive_and_process(int timeout_ms);
  /// Handle a parsed datagram (ACKs inbound requests, tracks the sequence).
  void process(const std::vector<std::uint8_t>& datagram);
  /// Send a CONNECTIONSTATE_REQUEST when the heartbeat interval has elapsed.
  void send_heartbeat_if_due();
  /// True when an unanswered heartbeat expired (pure query; maintain() then
  /// marks the tunnel lost).
  [[nodiscard]] bool heartbeat_overdue() const;
  /// Send a datagram to the configured device (the socket is not connected,
  /// so send() alone would fail with ENOTCONN).
  bool send_to_device(const std::vector<std::uint8_t>& datagram);

  TunnelOptions options_;
  int fd_ = -1;
  bool connected_ = false;
  bool lost_ = false;

  std::uint8_t channel_ = 0;
  std::uint16_t crd_address_ = 0;
  std::uint8_t tx_sequence_ = 0;
  std::uint8_t rx_expected_ = 0;
  bool rx_has_expected_ = false;
  std::uint8_t ack_wait_sequence_ = 0;
  bool ack_received_ = false;
  /// TUNNELLING_ACK status received for ack_wait_sequence_ (0x00 = accepted).
  std::uint8_t ack_status_ = 0;
  /// The device answered "busy, do not repeat": the telegram is dropped
  /// instead of being retransmitted or reported as sent.
  bool ack_rejected_ = false;

  /// A CONNECTIONSTATE_REQUEST is outstanding.  A device that never answers it
  /// is gone: without this the tunnel would look healthy forever while the bus
  /// is dead, and /r would keep the client on frozen values (AGENTS.md 9).
  bool state_pending_ = false;
  std::int64_t state_deadline_ms_ = 0;

  std::int64_t heartbeat_due_ms_ = 0;
  std::uint64_t telegram_count_ = 0;

  /// Telegrams received while waiting for an ACK (never dropped).
  std::deque<std::pair<std::uint16_t, std::vector<std::uint8_t>>> pending_;
};

}  // namespace cvknx
