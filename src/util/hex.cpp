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
 * @file hex.cpp
 * @brief Implementation of hex encoding/decoding.
 */

#include "hex.h"

#include <array>
#include <cctype>

namespace cvknx {

/// Decode a hex string into bytes; empty on any error.
///
/// The input comes from the `v`/`a` parameters of `/w`, i.e. from the network.
/// Rejecting the whole string instead of decoding the valid prefix is
/// intentional: a partially decoded value would be written to the bus as a
/// *different* value than the client asked for.
std::vector<uint8_t> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return {};  // a hex string always encodes whole bytes
  }
  std::vector<uint8_t> result;
  result.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    char hi = hex[i];
    char lo = hex[i + 1];
    if (std::isxdigit(static_cast<unsigned char>(hi)) == 0 ||
        std::isxdigit(static_cast<unsigned char>(lo)) == 0) {
      return {};
    }
    // isxdigit() above guarantees [0-9a-fA-F], so folding the case and
    // comparing against '9' covers every valid input with a single choice.
    auto nibble = [](char c) -> uint8_t {
      const auto lowered = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
      return static_cast<uint8_t>(lowered <= '9' ? lowered - '0' : lowered - 'a' + 10);
    };
    result.push_back(static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo)));
  }
  return result;
}

/// Encode bytes as lowercase hex — the only form CometVisu accepts.
/// Uppercase would still be valid hex, but the client compares and logs the
/// strings verbatim, so the output form is fixed here.
std::string hex_encode(const uint8_t* data, size_t len) {
  static constexpr std::array<char, 16> kHexChars = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    result.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
    result.push_back(kHexChars[data[i] & 0x0F]);
  }
  return result;
}

std::string hex_byte(uint8_t byte) {
  return hex_encode(&byte, 1);
}

}  // namespace cvknx
