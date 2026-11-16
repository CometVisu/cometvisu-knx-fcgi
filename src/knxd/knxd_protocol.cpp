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
 * @file knxd_protocol.cpp
 * @brief Implementation of the eibd wire protocol — address parsing, APDU
 *        building/parsing, and eibd message construction.
 *
 * All wire-format encodings (big-endian lengths, type-tagged payloads)
 * follow the eibd specification as implemented by knxd.
 *
 * @note A value that fits into 6 bits travels inside the second APDU octet;
 *       a longer value is placed in the following octets.  The encoder side
 *       (WriteHandler) therefore auto-promotes a 1-byte value above 0x3F to
 *       the multi-octet form, which is what every KNX device expects for
 *       1-byte DPTs such as DPT 5.010.
 */

#include "knxd_protocol.h"

#include <cstring>

namespace cvknx {

// ---- KnxGroupAddress ----

std::optional<KnxGroupAddress> KnxGroupAddress::from_string(std::string_view str) {
  KnxGroupAddress addr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char* p = str.data();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char* end = str.data() + str.size();

  auto parse_part = [&](uint8_t& out, int max_val) -> bool {
    if (p >= end || *p < '0' || *p > '9') {
      return false;
    }
    int val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      val = (val * 10) + (*p - '0');
      if (val > max_val) {
        return false;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ++p;
    }
    out = static_cast<uint8_t>(val);
    return true;
  };

  if (!parse_part(addr.main, 31)) {
    return std::nullopt;
  }
  if (p >= end || *p != '/') {
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  ++p;
  if (!parse_part(addr.middle, 7)) {
    return std::nullopt;
  }
  if (p >= end || *p != '/') {
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  ++p;
  if (!parse_part(addr.sub, 255)) {
    return std::nullopt;
  }
  if (p != end) {
    return std::nullopt;  // trailing garbage
  }

  return addr;
}

std::string KnxGroupAddress::to_string() const {
  return std::to_string(main) + "/" + std::to_string(middle) + "/" + std::to_string(sub);
}

uint16_t KnxGroupAddress::to_eibaddr() const {
  return static_cast<uint16_t>((static_cast<uint16_t>(main) << 11) |
                               (static_cast<uint16_t>(middle) << 8) | sub);
}

KnxGroupAddress KnxGroupAddress::from_eibaddr(uint16_t addr) {
  return KnxGroupAddress{.main = static_cast<uint8_t>((addr >> 11) & 0x1F),
                         .middle = static_cast<uint8_t>((addr >> 8) & 0x07),
                         .sub = static_cast<uint8_t>(addr & 0xFF)};
}

// ---- KnxAddress ----

namespace {

/// Default namespace, held in a function-local static so that no other
/// translation unit can observe it before it is constructed (static
/// initialization order).  Empty means "no prefix".
std::string& default_namespace_storage() {
  // The guard of a function-local static is taken once per process, so the
  // "already initialized" arm cannot appear inside a single test binary.
  static std::string ns;  // GCOV_EXCL_BR_LINE
  return ns;
}

}  // namespace

std::optional<KnxAddress> KnxAddress::from_cometvisu(std::string_view str) {
  const auto colon = str.find(':');
  if (colon == std::string_view::npos) {
    // No namespace: use the configured default namespace (empty by default).
    auto group = KnxGroupAddress::from_string(str);
    if (!group) {
      return std::nullopt;
    }
    return KnxAddress{.ns = default_namespace_storage(), .group = *group};
  }

  KnxAddress result;
  result.ns = std::string{str.substr(0, colon)};
  auto group = KnxGroupAddress::from_string(str.substr(colon + 1));
  if (!group) {
    return std::nullopt;
  }
  result.group = *group;
  return result;
}

std::string KnxAddress::to_cometvisu() const {
  if (ns.empty()) {
    return group.to_string();
  }
  return ns + ":" + group.to_string();
}

void KnxAddress::set_default_namespace(std::string_view ns) {
  default_namespace_storage() = ns;
}

std::string_view KnxAddress::get_default_namespace() {
  return default_namespace_storage();
}

// ---- APDU parsing ----

std::optional<Apdu> parse_apdu(std::span<const uint8_t> apdu) {
  // Two octets are the minimum for every APDU; a group value always has both.
  if (apdu.size() < 2) {
    return std::nullopt;
  }

  // TPCI type (bits 7..6): only "data" packets can carry a group value.
  // Connect / disconnect / connection-oriented frames are management traffic.
  if ((apdu[0] & 0xC0) != 0x00) {
    return std::nullopt;
  }

  // The 4-bit APCI is split: bits 1..0 of octet 1 are the high bits, bits 7..6
  // of octet 2 the low bits.
  const auto apci = static_cast<uint8_t>(((apdu[0] & 0x03) << 2) | (apdu[1] >> 6));

  Apdu result;
  switch (apci) {
    case static_cast<uint8_t>(ApduType::Read):
      // A_GroupValue_Read carries no value.
      result.type = ApduType::Read;
      return result;
    case static_cast<uint8_t>(ApduType::Response):
      result.type = ApduType::Response;
      break;
    case static_cast<uint8_t>(ApduType::Write):
      result.type = ApduType::Write;
      break;
    default:
      // Not a group value service (individual address, device descriptor,
      // property access, reserved APCI, ...).
      return std::nullopt;
  }

  if (apdu.size() == 2) {
    // Value of at most 6 bits, packed into the second octet.
    result.data.push_back(static_cast<uint8_t>(apdu[1] & 0x3F));
  } else {
    // Longer value: the payload starts at octet 3.
    result.data.assign(apdu.begin() + 2, apdu.end());
  }
  return result;
}

// ---- EIBD Messages ----

std::vector<uint8_t> build_eibd_message(uint16_t type, const std::vector<uint8_t>& data) {
  // Payload: [type_hi, type_lo] + [data...]
  const size_t payload_size = 2 + data.size();
  std::vector<uint8_t> result;
  result.reserve(2 + payload_size);

  // Length header (big-endian)
  result.push_back(static_cast<uint8_t>((payload_size >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(payload_size & 0xFF));

  // Type
  result.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(type & 0xFF));

  // Data
  result.insert(result.end(), data.begin(), data.end());

  return result;
}

bool parse_eibd_message(const std::vector<uint8_t>& raw, uint16_t& out_type,
                        std::vector<uint8_t>& out_data) {
  if (raw.size() < 4) {
    return false;  // need length(2) + type(2) minimum
  }

  // Verify length.  The payload must at least hold the 2-byte type, otherwise
  // the range below would be inverted (a malformed message must be rejected,
  // not turned into undefined behaviour).
  const auto payload_len = static_cast<uint16_t>((raw.at(0) << 8) | raw.at(1));
  if (payload_len < 2 || raw.size() < static_cast<size_t>(payload_len) + 2) {
    return false;
  }

  out_type = static_cast<uint16_t>((raw.at(2) << 8) | raw.at(3));
  out_data.assign(raw.begin() + 4, raw.begin() + 2 + payload_len);

  return true;
}

std::optional<std::vector<uint8_t>> try_extract_message(std::vector<uint8_t>& buffer) {
  if (buffer.size() < 2) {
    return std::nullopt;  // need at least the 2-byte length header
  }

  const auto payload_len = static_cast<uint16_t>((buffer.at(0) << 8) | buffer.at(1));
  const size_t total_needed = 2 + static_cast<size_t>(payload_len);

  if (buffer.size() < total_needed) {
    return std::nullopt;  // incomplete message — need more data
  }

  // Extract complete message
  std::vector<uint8_t> msg(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(total_needed));
  buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(total_needed));
  return msg;
}

// ---- Type-specific message builders ----
// These are the only parts that depend on knxd's <eibtypes.h> constants, so
// they disappear when the knxd transport is not built (KNXnet/IP needs none).

#ifdef KNX_WITH_KNXD_TRANSPORT
std::vector<uint8_t> build_open_groupcon(bool write_only) {
  // knxd >= 0.14 expects a 5-byte payload for all EIB_OPEN_* types:
  // [type:2][reserved/addr:2][write_only:1]
  // Use 0x01 for write-only (standard eibd sentinel), 0x00 for read-write.
  const uint8_t wo_byte = write_only ? 0x01 : 0x00;
  return build_eibd_message(EIB_OPEN_GROUPCON, {0x00, 0x00, wo_byte});
}

std::vector<uint8_t> build_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  // Payload: [type:2][dest_addr:2][apdu:N]
  std::vector<uint8_t> data;
  data.reserve(2 + apdu.size());
  data.push_back(static_cast<uint8_t>((group_addr >> 8) & 0xFF));
  data.push_back(static_cast<uint8_t>(group_addr & 0xFF));
  data.insert(data.end(), apdu.begin(), apdu.end());
  return build_eibd_message(EIB_GROUP_PACKET, data);
}
#endif  // KNX_WITH_KNXD_TRANSPORT

}  // namespace cvknx
