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
 * @file mock_bus_link.h
 * @brief Scriptable BusLink for unit and integration tests.
 */

#pragma once

#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/bus/bus_link.h"

namespace cvknx {

class MockBusLink : public BusLink {
public:
  MockBusLink() = default;

  [[nodiscard]] bool connect() override {
    return connect(std::string_view{"/run/knx"}) && open_group_socket(false);
  }
  [[nodiscard]] bool connect(std::string_view socket_path);
  void disconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;
  [[nodiscard]] std::string label() const override { return "mock bus link"; }

  /// Open the group socket (test helper, not part of BusLink).
  [[nodiscard]] bool open_group_socket(bool write_only);

  // --- test helpers ---
  void set_connection_success(bool success) { connection_success_ = success; }
  void enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu);
  struct SentPacket {
    uint16_t group_addr;
    std::vector<uint8_t> apdu;
  };
  [[nodiscard]] std::vector<SentPacket> sent_packets() const { return sent_packets_; }
  void reset();
  void set_telegram_count(uint64_t count) { telegram_count_ = count; }
  void set_send_fail_count(int count) { send_fail_count_ = count; }
  void set_connect_fail_count(int count) { connect_fail_count_ = count; }
  /// Number of upcoming wait_for_activity() calls that report a lost connection.
  void set_wait_error_count(int count) { wait_error_count_ = count; }
  /// Minimal interval between two sends, as a real transport would declare it.
  void set_min_send_interval_ms(int interval_ms) { min_send_interval_ms_ = interval_ms; }
  /// Drop the connection when a scripted send fails — the real transports do
  /// exactly that so the cache reader notices the dead link.
  void set_disconnect_on_send_failure(bool enable) { disconnect_on_send_failure_ = enable; }
  [[nodiscard]] int min_send_interval_ms() const override { return min_send_interval_ms_; }
  /// Number of connect() calls so far (used to detect reconnect behaviour).
  [[nodiscard]] int connect_calls() const { return connect_calls_; }
  /// Number of wait_for_activity() calls so far (used to detect busy loops).
  [[nodiscard]] int wait_calls() const { return wait_calls_; }

private:
  bool connected_ = false;
  bool group_socket_open_ = false;
  bool connection_success_ = true;
  int connect_fail_count_ = 0;
  int wait_error_count_ = 0;
  int connect_calls_ = 0;
  int wait_calls_ = 0;
  std::string last_socket_path_;
  std::queue<std::pair<uint16_t, std::vector<uint8_t>>> telegram_queue_;
  std::vector<SentPacket> sent_packets_;
  uint64_t telegram_count_ = 0;
  int send_fail_count_ = 0;
  int min_send_interval_ms_ = 0;
  bool disconnect_on_send_failure_ = false;
};

}  // namespace cvknx
