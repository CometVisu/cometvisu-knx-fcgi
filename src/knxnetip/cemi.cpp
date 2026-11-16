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
 * @file cemi.cpp
 * @brief cEMI frame encode/decode.
 */

#include "knxnetip/cemi.h"

namespace cvknx::cemi {
namespace {

/// Fixed part of an L_Data frame between the additional info and the APDU:
/// control 1 | control 2 | source (2) | destination (2) | length.
constexpr std::size_t kControlBlockSize = 7;

/// Encode a group-addressed L_Data frame with an explicit message code.
std::vector<std::uint8_t> encode_group_frame(std::uint8_t message_code, std::uint16_t source,
                                             std::uint16_t destination,
                                             std::span<const std::uint8_t> apdu) {
  // The length octet is (APDU octets - 1) and fits one byte.  A longer APDU
  // cannot be encoded: reject it instead of writing a truncated length that
  // would describe a different frame.
  if (apdu.empty() || apdu.size() > 256) {
    return {};
  }

  std::vector<std::uint8_t> frame;
  frame.reserve(2 + kControlBlockSize + apdu.size());
  frame.push_back(message_code);
  frame.push_back(0x00);  // additional info length
  frame.push_back(kControl1StandardFrame);
  frame.push_back(kControl2Group);
  frame.push_back(static_cast<std::uint8_t>((source >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(source & 0xFF));
  frame.push_back(static_cast<std::uint8_t>((destination >> 8) & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(destination & 0xFF));
  frame.push_back(static_cast<std::uint8_t>(apdu.size() - 1));
  frame.insert(frame.end(), apdu.begin(), apdu.end());
  return frame;
}

/// Destination address type (control field 2, bit 7).
constexpr std::uint8_t kControl2GroupMask = 0x80;

}  // namespace

std::vector<std::uint8_t> encode_group_request(std::uint16_t source, std::uint16_t destination,
                                               std::span<const std::uint8_t> apdu) {
  return encode_group_frame(kMessageCodeLDataReq, source, destination, apdu);
}

std::vector<std::uint8_t> encode_group_indication(std::uint16_t source, std::uint16_t destination,
                                                  std::span<const std::uint8_t> apdu) {
  return encode_group_frame(kMessageCodeLDataInd, source, destination, apdu);
}

std::optional<CemiFrame> parse_ldata_ind(std::span<const std::uint8_t> data) {
  // The message code comes first: a frame of any other type (confirmation,
  // request, bus monitor, raw frame, property access, reset, ...) has a
  // different layout and is irrelevant here.  Reject it before computing any
  // offset so a foreign layout can never be mistaken for a group value.
  if (data.size() < 2 || data[0] != kMessageCodeLDataInd) {
    return std::nullopt;
  }

  // Additional info: length octet plus that many transport-specific octets.
  const std::size_t body = 2 + static_cast<std::size_t>(data[1]);
  // Control block plus at least one APDU octet.
  if (data.size() < body + kControlBlockSize + 1) {
    return std::nullopt;
  }

  const std::uint8_t control2 = data[body + 1];
  if ((control2 & kControl2GroupMask) == 0) {
    return std::nullopt;  // individual (management) traffic is not for us
  }

  // Length octet: number of APDU octets minus one.  The frame must match it
  // exactly — a truncated or padded frame is rejected rather than guessed.
  const std::size_t apdu_len = static_cast<std::size_t>(data[body + 6]) + 1;
  if (data.size() != body + kControlBlockSize + apdu_len) {
    return std::nullopt;
  }

  CemiFrame frame;
  frame.source = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[body + 2]) << 8) |
                                            data[body + 3]);
  frame.destination = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[body + 4]) << 8) |
                                                 data[body + 5]);
  frame.apdu.assign(
      data.begin() + static_cast<std::ptrdiff_t>(body + kControlBlockSize),
      data.begin() + static_cast<std::ptrdiff_t>(body + kControlBlockSize + apdu_len));
  return frame;
}

}  // namespace cvknx::cemi
