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
 * @file cemi.h
 * @brief cEMI frame codec (KNX link layer) for the direct transports.
 *
 * cEMI (Common External Message Interface, KNX 03_08_01) wraps one link-layer
 * frame in a message code and a variable-length "additional information"
 * block:
 *
 * @code
 * [0]     message code        see MessageCode below
 * [1]     additional info length (0..255)
 * [2..]   additional info     transport specific, MUST be skipped
 * +0      control field 1     bit 7: 1 = standard frame, 0 = extended frame
 * +1      control field 2     bit 7: 1 = group destination, 0 = individual
 * +2..+3  source address      individual address of the sender
 * +4..+5  destination address group or individual
 * +6      length              number of APDU octets - 1
 * +7..    APDU                TPCI/APCI + data
 * @endcode
 *
 * The offsets after the control fields are **relative to the end of the
 * additional info block** — a frame with additional information is legal and
 * is emitted by couplers and by frames carrying PL/RF information.  Both the
 * standard and the extended frame format use this same cEMI layout: the
 * frame-type bit only records which wire format the frame had.
 *
 * Only `L_Data.ind` with a group destination carries a group value.  Every
 * other message code is irrelevant for this backend and is rejected as early
 * as it can be recognised — before any offset is computed, so a foreign
 * layout (bus monitor, raw frame, property access, reset, ...) can never be
 * misread as a group value.  `L_Data.con` in particular confirms *our own*
 * write and must be dropped, otherwise every write would be delivered twice
 * (AGENTS.md rule 1).
 *
 * Which message code a *write* uses depends on the transport:
 *   - `ip-tunnel` sends L_Data.req (0x11): the client asks the interface to
 *     transfer the frame, and the interface confirms with L_Data.con;
 *   - `ip-routing` sends L_Data.ind (0x29): on the IP backbone every
 *     participant acts as a router, and routers only accept indications.
 *     A ROUTING_INDICATION carrying L_Data.req is silently dropped by knxd
 *     and by real interfaces, so the write would never reach the bus.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cvknx::cemi {

/// cEMI message codes (KNX 03_08_01, message code octet).
///
/// The list is the complete set of link-layer and management messages a
/// KNXnet/IP interface can emit.  Only `kMessageCodeLDataInd` is bus traffic
/// for this backend; the others exist so that the parser and its tests can
/// name them.
enum MessageCode : std::uint8_t {
  kMessageCodeLRawReq = 0x10,
  kMessageCodeLDataReq = 0x11,
  kMessageCodeLPollDataReq = 0x13,
  kMessageCodeLBusyReq = 0x21,
  kMessageCodeLBusyInd = 0x23,
  kMessageCodeLPollDataCon = 0x25,
  kMessageCodeLDataInd = 0x29,
  kMessageCodeLBusmonInd = 0x2B,
  kMessageCodeLRawInd = 0x2D,
  kMessageCodeLDataCon = 0x2E,
  kMessageCodeLRawCon = 0x2F,
  kMessageCodeMResetInd = 0xF0,
  kMessageCodeMResetReq = 0xF1,
  kMessageCodeMPropWriteCon = 0xF5,
  kMessageCodeMPropWriteReq = 0xF6,
  kMessageCodeMPropInfoInd = 0xF7,
  kMessageCodeMFuncPropCommand = 0xF8,
  kMessageCodeMFuncPropStateRead = 0xF9,
  kMessageCodeMFuncPropCommandCon = 0xFA,
  kMessageCodeMPropReadCon = 0xFB,
  kMessageCodeMPropReadReq = 0xFC,
};

/// Control field 1 of a frame we send: standard frame, repeat not allowed,
/// system broadcast, low priority.
inline constexpr std::uint8_t kControl1StandardFrame = 0xBC;
/// Control field 2 of a frame we send: group destination, hop count 6.
inline constexpr std::uint8_t kControl2Group = 0xE0;

/// A decoded group-value link-layer frame.
struct CemiFrame {
  std::uint16_t source = 0;        ///< individual address of the sender
  std::uint16_t destination = 0;   ///< group address
  std::vector<std::uint8_t> apdu;  ///< TPCI/APCI + data
};

/// Byte buffer produced by the encoders (named so call sites read clearly).
using Bytes = std::vector<std::uint8_t>;

/// Encode an L_Data.req for a group value (tunnelling clients).
/// @param source Individual address (0x0000 = let the device fill it in).
/// @param destination 16-bit group address.
/// @param apdu TPCI/APCI + data (at least one byte).
/// @return the cEMI frame, or an empty vector when @p apdu is empty (the
///         length byte would underflow).
[[nodiscard]] std::vector<std::uint8_t> encode_group_request(std::uint16_t source,
                                                             std::uint16_t destination,
                                                             std::span<const std::uint8_t> apdu);

/// Encode an L_Data.ind for a group value (routing participants).
///
/// The source must be a valid individual address: a KNXnet/IP router rejects
/// a frame from 0.0.0, and a source that belongs to one of its own tunnel
/// clients (knxd does this) as well.
/// @return the cEMI frame, or an empty vector when @p apdu is empty.
[[nodiscard]] std::vector<std::uint8_t> encode_group_indication(std::uint16_t source,
                                                                std::uint16_t destination,
                                                                std::span<const std::uint8_t> apdu);

/// Parse an L_Data.ind with a group destination.
///
/// Frames that are irrelevant for the backend are rejected as early as
/// possible: a different message code first (no offset is computed at all),
/// then a destination that is not a group address, then any length mismatch.
///
/// @return the frame, or std::nullopt for any other message code, a truncated
///         or padded frame, or an individual destination.
[[nodiscard]] std::optional<CemiFrame> parse_ldata_ind(std::span<const std::uint8_t> data);

}  // namespace cvknx::cemi
