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
 * @file bus_connector.cpp
 * @brief Bounded connect-with-retry helper for any BusLink.
 */

#include "bus/bus_connector.h"

#include <time.h>

#include <array>
#include <cstddef>
#include <iostream>

namespace cvknx {
namespace {

/// Sleep for @p delay_ms milliseconds (no obsolete usleep()).
void sleep_ms(int delay_ms) {
  if (delay_ms <= 0) {
    return;
  }
  struct timespec request {};
  request.tv_sec = delay_ms / 1000;
  request.tv_nsec = static_cast<long>(delay_ms % 1000) * 1000000L;
  (void)::nanosleep(&request, nullptr);
}

}  // namespace

bool connect_bus_with_retry(BusLink& link, std::span<const int> delays_ms) {
  static constexpr std::array<int, 5> kDefaultDelaysMs = {500, 1000, 2000, 4000, 8000};
  const std::span<const int> delays =
      delays_ms.empty() ? std::span<const int>{kDefaultDelaysMs} : delays_ms;

  const std::size_t total_attempts = delays.size() + 1;
  for (std::size_t attempt = 1; attempt <= total_attempts; ++attempt) {
    if (link.connect()) {
      if (attempt > 1) {
        std::cout << "[INFO] Bus connection to " << link.label() << " succeeded after " << attempt
                  << " attempts\n";
        std::cout << std::flush;
      }
      return true;
    }
    if (attempt < total_attempts) {
      const int delay = delays[attempt - 1];
      std::cerr << "[WARN] Bus connection attempt " << attempt << "/" << total_attempts
                << " failed, retrying in " << delay << "ms...\n";
      std::cerr << std::flush;
      sleep_ms(delay);
    }
  }

  std::cerr << "[ERROR] Bus connection failed after " << total_attempts << " attempts, giving up\n";
  std::cerr << std::flush;
  return false;
}

}  // namespace cvknx
