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
 * @file knxnetip_frame.h
 * @brief KNXnet/IP framing: header, HPAI and service-specific messages.
 *
 * Header (6 bytes):
 * @code
 * [0] 0x06 header length
 * [1] 0x10 protocol version 1.0
 * [2..3] service type (big endian)
 * [4..5] total length incl. header (big endian)
 * @endcode
 *
 * HPAI (IPv4/UDP, 8 bytes):
 * @code
 * [0] 0x08 length | [1] 0x01 IPv4/UDP | [2..5] IPv4 address | [6..7] port
 * @endcode
 *
 * Unknown service types are ignored by the receiver, not treated as errors
 * (routers emit extra messages).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cvknx::knxnetip {

inline constexpr std::uint8_t kHeaderLength = 0x06;
inline constexpr std::uint8_t kProtocolVersion = 0x10;
inline constexpr std::uint16_t kDefaultPort = 3671;
inline constexpr std::uint8_t kHostProtocolIpv4Udp = 0x01;
inline constexpr std::uint8_t kConnectionTypeTunnel = 0x04;
inline constexpr std::uint8_t kKnxLayerLink = 0x02;
/// CRI structure length: length byte + connection type + KNX layer + reserved.
inline constexpr std::uint8_t kCriLength = 0x04;
inline constexpr std::uint8_t kStructuringNoMoreInfo = 0x00;

enum class ServiceType : std::uint16_t {
  SearchRequest = 0x0201,
  SearchResponse = 0x0202,
  DescriptionRequest = 0x0203,
  DescriptionResponse = 0x0204,
  ConnectRequest = 0x0205,
  ConnectResponse = 0x0206,
  ConnectionStateRequest = 0x0207,
  ConnectionStateResponse = 0x0208,
  DisconnectRequest = 0x0209,
  DisconnectResponse = 0x020A,
  TunnellingRequest = 0x0420,
  TunnellingAck = 0x0421,
  // KNXnet/IP chapter 3/8: the routing services live in the 0x05xx range.
  // 0x0330 is *not* ROUTING_INDICATION — a router silently drops a frame that
  // uses it, which is why the value is called out here.
  RoutingIndication = 0x0530,
  RoutingLostMessage = 0x0531,
  RoutingBusy = 0x0532,
};

/// CONNECT_RESPONSE status codes (subset that matters in practice).
enum class ConnectStatus : std::uint8_t {
  NoError = 0x00,
  HostProtocolType = 0x01,
  ProtocolVersion = 0x02,
  SequenceNumber = 0x03,
  ConnectionId = 0x04,
  ConnectionType = 0x05,
  ConnectionOption = 0x06,
  NoMoreConnections = 0x07,
  NoMoreUniqueConnections = 0x08,
  DataConnection = 0x09,
  KnxConnection = 0x0A,
  TunnellingLayer = 0x0B,
};

/// Human readable text for a status byte (log lines must name the tunnel limit).
[[nodiscard]] const char* connect_status_text(std::uint8_t status);

/// IPv4 endpoint in HPAI form.  `address` is host byte order.
struct Hpai {
  std::uint32_t address = 0;
  std::uint16_t port = 0;
  bool operator==(const Hpai&) const = default;
};

/// A parsed KNXnet/IP frame.
struct Frame {
  ServiceType service = ServiceType::SearchRequest;
  std::vector<std::uint8_t> body;
};

// ---- Generic framing ----

[[nodiscard]] std::vector<std::uint8_t> build_frame(ServiceType service,
                                                    const std::vector<std::uint8_t>& body);

/// Parse and validate a complete datagram.
/// @return std::nullopt for a wrong header length/version, a length mismatch
///         or an unknown service type.
[[nodiscard]] std::optional<Frame> parse_frame(const std::uint8_t* data, std::size_t size);
[[nodiscard]] std::optional<Frame> parse_frame(std::span<const std::uint8_t> data);

// ---- HPAI ----

[[nodiscard]] std::vector<std::uint8_t> encode_hpai(const Hpai& hpai);
[[nodiscard]] std::optional<Hpai> parse_hpai(const std::uint8_t* data, std::size_t size);

/// DNS-free IPv4 helpers ("192.168.1.10" <-> host-order uint32).
[[nodiscard]] std::optional<std::uint32_t> ipv4_from_string(std::string_view text);
[[nodiscard]] std::string ipv4_to_string(std::uint32_t address);

// ---- Tunnelling connection management ----

/// CONNECT_REQUEST: HPAI control | HPAI data | CRI
/// (`0x04` length, `0x04` tunnel, `0x02` link layer, `0x00` reserved).
/// The length byte is mandatory: a device validates `body[16] == size - 16`
/// and ignores a request without it.
[[nodiscard]] std::vector<std::uint8_t> build_connect_request(const Hpai& control,
                                                              const Hpai& data);

struct ConnectResponse {
  std::uint8_t channel = 0;
  std::uint8_t status = 0;
  Hpai endpoint;
  std::uint16_t crd_address = 0;  ///< individual address we must use as source
};

[[nodiscard]] std::optional<ConnectResponse> parse_connect_response(
    std::span<const std::uint8_t> frame);

/// CONNECTIONSTATE_REQUEST / DISCONNECT_REQUEST share the same body.
[[nodiscard]] std::vector<std::uint8_t> build_connectionstate_request(std::uint8_t channel,
                                                                      const Hpai& control);
[[nodiscard]] std::vector<std::uint8_t> build_disconnect_request(std::uint8_t channel,
                                                                 const Hpai& control);

/// CONNECTIONSTATE_RESPONSE / DISCONNECT_RESPONSE share the same body.
struct StatusResponse {
  std::uint8_t channel = 0;
  std::uint8_t status = 0;
};

[[nodiscard]] std::optional<StatusResponse> parse_status_response(
    std::span<const std::uint8_t> frame, ServiceType expected);

// ---- Tunnelling data ----

struct TunnellingRequest {
  std::uint8_t channel = 0;
  std::uint8_t sequence = 0;
  std::vector<std::uint8_t> cemi;
};

[[nodiscard]] std::vector<std::uint8_t> build_tunnelling_request(
    std::uint8_t channel, std::uint8_t sequence, const std::vector<std::uint8_t>& cemi);
[[nodiscard]] std::optional<TunnellingRequest> parse_tunnelling_request(
    std::span<const std::uint8_t> frame);

struct TunnellingAck {
  std::uint8_t channel = 0;
  std::uint8_t sequence = 0;
  std::uint8_t status = 0;
};

/// TUNNELLING_ACK body: `0x04` structure length | channel | sequence | status.
/// (Unlike the request there is no reserved byte: the status takes its place.)
[[nodiscard]] std::vector<std::uint8_t> build_tunnelling_ack(std::uint8_t channel,
                                                             std::uint8_t sequence,
                                                             std::uint8_t status = 0);
[[nodiscard]] std::optional<TunnellingAck> parse_tunnelling_ack(
    std::span<const std::uint8_t> frame);

// ---- Routing ----

/// ROUTING_INDICATION body is a bare cEMI frame.
[[nodiscard]] std::vector<std::uint8_t> build_routing_indication(
    const std::vector<std::uint8_t>& cemi);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> parse_routing_indication(
    std::span<const std::uint8_t> frame);

/// Zero-copy variant of parse_routing_indication(): validates the frame header
/// and returns a view of the cEMI payload inside @p frame.  The view is valid
/// only as long as @p frame is (the caller owns the receive buffer).
[[nodiscard]] std::optional<std::span<const std::uint8_t>> parse_routing_payload(
    std::span<const std::uint8_t> frame);

// ---- Discovery ----

/// SEARCH_REQUEST: a single HPAI control endpoint.
[[nodiscard]] std::vector<std::uint8_t> build_search_request(const Hpai& control);

/// Device information from a SEARCH_RESPONSE hardware DIB.
struct SearchResponse {
  Hpai control;
  Hpai endpoint;
  std::string device_name;
  std::uint16_t individual_address = 0;
  std::uint32_t multicast_address = 0;
};

[[nodiscard]] std::optional<SearchResponse> parse_search_response(
    std::span<const std::uint8_t> frame);

}  // namespace cvknx::knxnetip
