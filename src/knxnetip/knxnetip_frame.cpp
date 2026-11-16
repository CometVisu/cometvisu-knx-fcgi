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
 * @file knxnetip_frame.cpp
 * @brief KNXnet/IP framing implementation.  All lengths are derived from the
 *        actual buffer size; every parser validates before reading.
 */

#include "knxnetip/knxnetip_frame.h"

#include <arpa/inet.h>

#include <cstring>
#include <limits>

namespace cvknx::knxnetip {
namespace {

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

/// True for the service types this backend understands.
///
/// Anything else is dropped before its body is interpreted: routers and
/// gateways emit additional services (e.g. `SEARCH_REQUEST_EXTENDED` or
/// `ROUTING_SYSTEM_BROADCAST`), and a body read with the wrong layout is a
/// source of garbage values.
bool is_known_service(std::uint16_t value) {
  switch (static_cast<ServiceType>(value)) {
    case ServiceType::SearchRequest:
    case ServiceType::SearchResponse:
    case ServiceType::DescriptionRequest:
    case ServiceType::DescriptionResponse:
    case ServiceType::ConnectRequest:
    case ServiceType::ConnectResponse:
    case ServiceType::ConnectionStateRequest:
    case ServiceType::ConnectionStateResponse:
    case ServiceType::DisconnectRequest:
    case ServiceType::DisconnectResponse:
    case ServiceType::TunnellingRequest:
    case ServiceType::TunnellingAck:
    case ServiceType::RoutingIndication:
    case ServiceType::RoutingLostMessage:
    case ServiceType::RoutingBusy:
      return true;
  }
  return false;
}

}  // namespace

const char* connect_status_text(std::uint8_t status) {
  switch (static_cast<ConnectStatus>(status)) {
    case ConnectStatus::NoError:
      return "no error";
    case ConnectStatus::HostProtocolType:
      return "host protocol type not supported";
    case ConnectStatus::ProtocolVersion:
      return "protocol version not supported";
    case ConnectStatus::SequenceNumber:
      return "sequence number error";
    case ConnectStatus::ConnectionId:
      return "unknown connection id";
    case ConnectStatus::ConnectionType:
      return "connection type not supported";
    case ConnectStatus::ConnectionOption:
      return "connection option not supported";
    case ConnectStatus::NoMoreConnections:
      return "no more tunnelling connections available (device tunnel limit reached)";
    case ConnectStatus::NoMoreUniqueConnections:
      return "no more unique tunnelling connections available (device tunnel limit reached)";
    case ConnectStatus::DataConnection:
      return "data connection error";
    case ConnectStatus::KnxConnection:
      return "KNX connection error";
    case ConnectStatus::TunnellingLayer:
      return "tunnelling layer not supported";
  }
  return "unknown status";
}

std::vector<std::uint8_t> build_frame(ServiceType service, const std::vector<std::uint8_t>& body) {
  // The total length field is 16 bits.  Truncating it would emit a frame that
  // every receiver drops (and that no longer describes its own contents) —
  // reject the body instead.
  if (body.size() > std::numeric_limits<std::uint16_t>::max() - kHeaderLength) {
    return {};
  }
  const auto total = static_cast<std::uint16_t>(kHeaderLength + body.size());
  std::vector<std::uint8_t> frame;
  frame.reserve(total);
  frame.push_back(kHeaderLength);
  frame.push_back(kProtocolVersion);
  append_u16(frame, static_cast<std::uint16_t>(service));
  append_u16(frame, total);
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}  // GCOV_EXCL_LINE - closing brace, no statement

std::optional<Frame> parse_frame(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return std::nullopt;
  }
  if (size < kHeaderLength) {
    return std::nullopt;
  }
  if (data[0] != kHeaderLength || data[1] != kProtocolVersion) {
    return std::nullopt;
  }
  const std::uint16_t service = read_u16(data + 2);
  const std::uint16_t total_length = read_u16(data + 4);
  if (total_length != size) {
    return std::nullopt;
  }
  // Unknown service types are ignored (routers emit extras) — callers simply
  // get std::nullopt and drop the datagram.
  if (!is_known_service(service)) {
    return std::nullopt;
  }

  Frame frame;
  frame.service = static_cast<ServiceType>(service);
  frame.body.assign(data + kHeaderLength, data + size);
  return frame;
}

std::optional<Frame> parse_frame(std::span<const std::uint8_t> data) {
  return parse_frame(data.data(), data.size());
}

std::vector<std::uint8_t> encode_hpai(const Hpai& hpai) {
  std::vector<std::uint8_t> out;
  out.reserve(8);
  out.push_back(0x08);
  out.push_back(kHostProtocolIpv4Udp);
  const std::uint32_t network_address = htonl(hpai.address);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&network_address);
  out.insert(out.end(), bytes, bytes + 4);
  append_u16(out, hpai.port);
  return out;
}  // GCOV_EXCL_LINE - closing brace, no statement

std::optional<Hpai> parse_hpai(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return std::nullopt;
  }
  if (size < 8) {
    return std::nullopt;
  }
  if (data[0] != 0x08) {
    return std::nullopt;
  }
  if (data[1] != kHostProtocolIpv4Udp) {
    return std::nullopt;
  }
  Hpai hpai;
  hpai.address = (static_cast<std::uint32_t>(data[2]) << 24) |
                 (static_cast<std::uint32_t>(data[3]) << 16) |
                 (static_cast<std::uint32_t>(data[4]) << 8) | static_cast<std::uint32_t>(data[5]);
  hpai.port = read_u16(data + 6);
  return hpai;
}

std::optional<std::uint32_t> ipv4_from_string(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::string copy{text};
  struct in_addr addr {};
  if (::inet_pton(AF_INET, copy.c_str(), &addr) != 1) {
    return std::nullopt;
  }
  return ntohl(addr.s_addr);
}

std::string ipv4_to_string(std::uint32_t address) {
  struct in_addr addr {};
  addr.s_addr = htonl(address);
  char buffer[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)) == nullptr) {  // GCOV_EXCL_BR_LINE
    return {};  // GCOV_EXCL_LINE - only fails for a bad family
  }
  return buffer;
}

std::vector<std::uint8_t> build_connect_request(const Hpai& control, const Hpai& data) {
  std::vector<std::uint8_t> body = encode_hpai(control);
  const auto data_hpai = encode_hpai(data);
  body.insert(body.end(), data_hpai.begin(), data_hpai.end());
  body.push_back(kCriLength);
  body.push_back(kConnectionTypeTunnel);
  body.push_back(kKnxLayerLink);
  body.push_back(0x00);  // reserved
  return build_frame(ServiceType::ConnectRequest, body);
}

std::optional<ConnectResponse> parse_connect_response(std::span<const std::uint8_t> frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->service != ServiceType::ConnectResponse) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  // 2 (channel, status) + 8 (HPAI) + 4 (CRD) is the smallest legal body; a
  // shorter one would make the CRD read below run past the end.
  if (body.size() < 14) {
    return std::nullopt;
  }
  ConnectResponse response;
  response.channel = body[0];
  response.status = body[1];
  const auto endpoint = parse_hpai(body.data() + 2, body.size() - 2);
  if (!endpoint) {
    return std::nullopt;
  }
  response.endpoint = *endpoint;
  // CRD: 0x04 0x04 <individual address hi> <lo>
  response.crd_address = read_u16(body.data() + 12);
  return response;
}

// CONNECTIONSTATE_REQUEST and DISCONNECT_REQUEST share one body layout
// (03_08_01 5.4/5.5): channel, reserved byte, control HPAI.  They are kept as
// two functions because the service code in the header differs and a caller
// must not be able to confuse the two.
std::vector<std::uint8_t> build_connectionstate_request(std::uint8_t channel, const Hpai& control) {
  std::vector<std::uint8_t> body;
  body.push_back(channel);
  body.push_back(0x00);  // reserved
  const auto hpai = encode_hpai(control);
  body.insert(body.end(), hpai.begin(), hpai.end());
  return build_frame(ServiceType::ConnectionStateRequest, body);
}

std::vector<std::uint8_t> build_disconnect_request(std::uint8_t channel, const Hpai& control) {
  std::vector<std::uint8_t> body;
  body.push_back(channel);
  body.push_back(0x00);  // reserved
  const auto hpai = encode_hpai(control);
  body.insert(body.end(), hpai.begin(), hpai.end());
  return build_frame(ServiceType::DisconnectRequest, body);
}

std::optional<StatusResponse> parse_status_response(std::span<const std::uint8_t> frame,
                                                    ServiceType expected) {
  const auto parsed = parse_frame(frame);
  if (!parsed || parsed->service != expected || parsed->body.size() < 2) {
    return std::nullopt;
  }
  return StatusResponse{parsed->body[0], parsed->body[1]};
}

std::vector<std::uint8_t> build_tunnelling_request(std::uint8_t channel, std::uint8_t sequence,
                                                   const std::vector<std::uint8_t>& cemi) {
  std::vector<std::uint8_t> body;
  body.reserve(4 + cemi.size());
  body.push_back(0x04);  // structure length
  body.push_back(channel);
  body.push_back(sequence);
  body.push_back(kStructuringNoMoreInfo);
  body.insert(body.end(), cemi.begin(), cemi.end());
  return build_frame(ServiceType::TunnellingRequest, body);
}

std::optional<TunnellingRequest> parse_tunnelling_request(std::span<const std::uint8_t> frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->service != ServiceType::TunnellingRequest) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 5 || body[0] != 0x04) {
    return std::nullopt;
  }
  TunnellingRequest request;
  request.channel = body[1];
  request.sequence = body[2];
  request.cemi.assign(body.begin() + 4, body.end());
  return request;
}

std::vector<std::uint8_t> build_tunnelling_ack(std::uint8_t channel, std::uint8_t sequence,
                                               std::uint8_t status) {
  // 0x04 = structure length: length, channel, sequence, status.  Unlike the
  // request there is no reserved byte — the status takes its place, which is
  // what knxd, Calimero and every real interface send.
  return build_frame(ServiceType::TunnellingAck, {0x04, channel, sequence, status});
}

std::optional<TunnellingAck> parse_tunnelling_ack(std::span<const std::uint8_t> frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->service != ServiceType::TunnellingAck) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() != 4 || body[0] != 0x04) {
    return std::nullopt;
  }
  return TunnellingAck{body[1], body[2], body[3]};
}

std::vector<std::uint8_t> build_routing_indication(const std::vector<std::uint8_t>& cemi) {
  return build_frame(ServiceType::RoutingIndication, cemi);
}

std::optional<std::vector<std::uint8_t>> parse_routing_indication(
    std::span<const std::uint8_t> frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->service != ServiceType::RoutingIndication) {
    return std::nullopt;
  }
  if (parsed->body.empty()) {
    return std::nullopt;
  }
  return parsed->body;
}

std::optional<std::span<const std::uint8_t>> parse_routing_payload(
    std::span<const std::uint8_t> frame) {
  // Same checks as parse_frame(), but the payload is returned as a view into
  // the caller's receive buffer instead of a copy.
  if (frame.size() < kHeaderLength) {
    return std::nullopt;
  }
  if (frame[0] != kHeaderLength || frame[1] != kProtocolVersion) {
    return std::nullopt;
  }
  if (read_u16(frame.data() + 2) != static_cast<std::uint16_t>(ServiceType::RoutingIndication)) {
    return std::nullopt;
  }
  if (read_u16(frame.data() + 4) != frame.size()) {
    return std::nullopt;
  }
  const auto payload = frame.subspan(kHeaderLength);
  if (payload.empty()) {
    return std::nullopt;
  }
  return payload;
}

std::vector<std::uint8_t> build_search_request(const Hpai& control) {
  return build_frame(ServiceType::SearchRequest, encode_hpai(control));
}

std::optional<SearchResponse> parse_search_response(std::span<const std::uint8_t> frame) {
  const auto parsed = parse_frame(frame);
  if (!parsed) {
    return std::nullopt;
  }
  if (parsed->service != ServiceType::SearchResponse) {
    return std::nullopt;
  }
  const auto& body = parsed->body;
  if (body.size() < 16) {
    return std::nullopt;
  }
  const auto control = parse_hpai(body.data(), body.size());
  if (!control) {
    return std::nullopt;
  }
  const auto endpoint = parse_hpai(body.data() + 8, body.size() - 8);
  if (!endpoint) {
    return std::nullopt;
  }

  SearchResponse response;
  response.control = *control;
  response.endpoint = *endpoint;

  // DIBs follow the two HPAIs; look for the device information DIB (0x01).
  std::size_t offset = 16;
  while (offset + 2 <= body.size()) {
    const std::size_t dib_length = body[offset];
    const std::uint8_t dib_type = body[offset + 1];
    if (dib_length < 2) {
      break;
    }
    if (offset + dib_length > body.size()) {
      break;
    }
    if (dib_type != 0x01) {
      offset += dib_length;
      continue;
    }
    if (dib_length >= 54) {
      const std::uint8_t* dib = body.data() + offset + 2;
      response.individual_address = read_u16(dib + 2);
      response.multicast_address = (static_cast<std::uint32_t>(dib[12]) << 24) |
                                   (static_cast<std::uint32_t>(dib[13]) << 16) |
                                   (static_cast<std::uint32_t>(dib[14]) << 8) |
                                   static_cast<std::uint32_t>(dib[15]);
      // 30-byte name, NUL padded.
      const char* name = reinterpret_cast<const char*>(dib + 22);
      std::size_t name_len = 0;
      while (name_len < 30 && name[name_len] != '\0') {
        ++name_len;
      }
      response.device_name.assign(name, name_len);
    }
    offset += dib_length;
  }
  return response;
}

}  // namespace cvknx::knxnetip
