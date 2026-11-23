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
 * @file mock_bus_link.cpp
 * @brief Scriptable BusLink for unit and integration tests.
 */

#include "mock_bus_link.h"

namespace cvknx {

bool MockBusLink::connect(std::string_view socket_path) {
  last_socket_path_ = socket_path;
  connect_calls_++;
  if (connect_fail_count_ > 0) {
    connect_fail_count_--;
    connected_ = false;
    return false;
  }
  connected_ = connection_success_;
  return connected_;
}

void MockBusLink::disconnect() {
  connected_ = false;
  group_socket_open_ = false;
  while (!telegram_queue_.empty()) {
    telegram_queue_.pop();
  }
}

bool MockBusLink::is_connected() const {
  return connected_;
}

bool MockBusLink::open_group_socket(bool /*write_only*/) {
  if (!connected_) {
    return false;
  }
  group_socket_open_ = connection_success_;
  return group_socket_open_;
}

bool MockBusLink::send_group_packet(uint16_t addr, const std::vector<uint8_t>& apdu) {
  if (!connected_ || !group_socket_open_) {
    return false;
  }
  if (send_fail_count_ > 0) {
    send_fail_count_--;
    if (disconnect_on_send_failure_) {
      connected_ = false;
      group_socket_open_ = false;
    }
    return false;
  }
  sent_packets_.push_back({addr, apdu});
  return true;
}

bool MockBusLink::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (telegram_queue_.empty()) {
    return false;
  }
  auto& f = telegram_queue_.front();
  out_group_addr = f.first;
  out_apdu = f.second;
  telegram_queue_.pop();
  telegram_count_++;
  return true;
}

int MockBusLink::get_fd() const {
  return -1;
}

uint64_t MockBusLink::get_telegram_count() const {
  return telegram_count_;
}

void MockBusLink::set_nonblocking(bool /*enable*/) {}

BusLink::WaitResult MockBusLink::wait_for_activity(int /*timeout_ms*/) {
  wait_calls_++;
  if (!connected_) {
    return WaitResult::Error;
  }
  if (wait_error_count_ > 0) {
    wait_error_count_--;
    return WaitResult::Error;
  }
  return telegram_queue_.empty() ? WaitResult::Timeout : WaitResult::GroupData;
}

void MockBusLink::enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu) {
  telegram_queue_.emplace(addr, apdu);
}

void MockBusLink::reset() {
  connected_ = false;
  group_socket_open_ = false;
  connection_success_ = true;
  last_socket_path_.clear();
  while (!telegram_queue_.empty()) {
    telegram_queue_.pop();
  }
  sent_packets_.clear();
  telegram_count_ = 0;
  send_fail_count_ = 0;
  connect_fail_count_ = 0;
  wait_error_count_ = 0;
  connect_calls_ = 0;
  wait_calls_ = 0;
  min_send_interval_ms_ = 0;
  disconnect_on_send_failure_ = false;
}

}  // namespace cvknx
