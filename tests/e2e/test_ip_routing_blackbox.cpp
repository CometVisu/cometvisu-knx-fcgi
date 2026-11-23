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
 * @file test_ip_routing_blackbox.cpp
 * @brief Black-box tests of `KNX_TRANSPORT=ip-routing` against a real knxd.
 *
 * Topology of every test:
 *
 * @code
 *   knxtool groupswrite ──► knxd ──(multicast, ip: router)──► backend ──► /r (FastCGI)
 *                              ▲                                  │
 *                              └──────── L_Data.ind ◄──────────────┘ (/w)
 *   a test process joined to the group monitors every frame on the wire
 * @endcode
 *
 * knxd is started with the `ip:` layer-2 driver on the loopback interface, so
 * it is a real KNXnet/IP router for the group; the backend joins the same
 * group.  The loopback interface is used because knxd ignores datagrams whose
 * source is its own IP:port ("ignore multicast packets sent by ourselves") —
 * two processes on one host must therefore use different source addresses.
 *
 * The monitor is the independent witness of what the backend puts on the
 * wire; knxd's group cache is the witness that a write really reached the bus.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "knxnetip_blackbox.h"

namespace {

using cvknx::blackbox::Backend;
using cvknx::blackbox::BackendOptions;
using cvknx::blackbox::Bytes;
using cvknx::blackbox::FcgiResponse;
using cvknx::blackbox::Knxd;
using cvknx::blackbox::KnxdMode;
using cvknx::blackbox::KnxdOptions;
using cvknx::blackbox::RoutingFrame;
using cvknx::blackbox::RoutingMonitor;
using Clock = std::chrono::steady_clock;

/// Default individual address the backend advertises for routing writes.
constexpr std::uint16_t kDefaultSourceIa = 0xFFFA;  // 15.15.250
constexpr std::uint16_t kRoutingIndicationService = 0x0530;

class IpRoutingBlackBox : public ::testing::Test {
protected:
  void SetUp() override {
    if (!cvknx::blackbox::knxd_available()) {
      GTEST_SKIP() << "knxd/knxtool are not installed";
    }
    group_ = cvknx::blackbox::unique_multicast_group();
    port_ = cvknx::blackbox::free_udp_port();
    ASSERT_NE(port_, 0) << "no free UDP port";

    KnxdOptions knxd_options;
    knxd_options.mode = KnxdMode::Router;
    knxd_options.group = group_;
    knxd_options.port = port_;
    knxd_options.iface = "lo";
    knxd_ = std::make_unique<Knxd>(knxd_options);
    ASSERT_TRUE(knxd_->start()) << knxd_->log();

    StartBackend();

    monitor_ = std::make_unique<RoutingMonitor>(group_, port_, "127.0.0.1");
    ASSERT_TRUE(monitor_->open());
  }

  void TearDown() override {
    if (backend_) {
      backend_->stop();
    }
    if (knxd_) {
      knxd_->stop();
    }
  }

  /// Start (or restart) the backend process with the routing environment.
  void StartBackend(int longpoll_seconds = 20, const cvknx::blackbox::Env& extra = {},
                    const std::string& individual_address = "15.15.250") {
    if (backend_) {
      backend_->stop();
    }
    BackendOptions options;
    // Several workers: a blocked long-poll must never keep /w from being
    // served (production defaults to 20).  Two workers stay reserved for the
    // short requests, so 8 workers allow 6 concurrent long-polls
    // (EveryBlockedClientReceivesTheSameTelegram).
    options.workers = 8;
    options.longpoll_seconds = longpoll_seconds;
    options.env = {{"KNX_TRANSPORT", "ip-routing"},
                   {"KNX_IP_MULTICAST", group_},
                   {"KNX_IP_PORT", std::to_string(port_)},
                   {"KNX_IP_IFACE", "lo"}};
    if (!individual_address.empty()) {
      options.env.emplace_back("KNX_IP_INDIVIDUAL_ADDR", individual_address);
    }
    for (const auto& entry : extra) {
      options.env.push_back(entry);
    }
    backend_ = std::make_unique<Backend>(options);
    ASSERT_TRUE(backend_->start()) << backend_->log();
  }

  [[nodiscard]] std::string group_text(int sub) const { return "1/2/" + std::to_string(sub); }
  [[nodiscard]] std::uint16_t group_value(int sub) const {
    return cvknx::blackbox::group_address_value(group_text(sub));
  }

  [[nodiscard]] FcgiResponse read(const std::string& query) const {
    return cvknx::blackbox::fcgi_request(backend_->socket_path(),
                                         "/r?" + cvknx::blackbox::with_session(query));
  }
  [[nodiscard]] FcgiResponse write(const std::string& group, const std::string& value) const {
    return cvknx::blackbox::fcgi_request(
        backend_->socket_path(),
        "/w?" + cvknx::blackbox::with_session("a=" + group + "&v=" + value));
  }
  [[nodiscard]] FcgiResponse cache_only(int sub) const {
    return read("a=" + group_text(sub) + "&t=-1");
  }

  /// Poll a cache-only read until @p expected is the reported data object.
  /// A telegram injected through knxd needs a moment to reach the backend;
  /// polling keeps the test deterministic on a loaded machine.
  [[nodiscard]] FcgiResponse wait_for_data(const std::string& query, const std::string& expected,
                                           int timeout_ms = 3000) const {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    FcgiResponse response = read(query);
    while (Clock::now() < deadline) {
      if (cvknx::blackbox::response_data_object(response) == expected) {
        return response;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      response = read(query);
    }
    return response;
  }

  [[nodiscard]] FcgiResponse wait_for_value(int sub, const std::string& expected,
                                            int timeout_ms = 3000) const {
    return wait_for_data("a=" + group_text(sub) + "&t=-1", expected, timeout_ms);
  }

  /// Frames the backend itself put on the multicast group for @p group.
  [[nodiscard]] std::vector<RoutingFrame> backend_frames(std::uint16_t group, int timeout_ms = 1500,
                                                         int expected = -1) {
    std::vector<RoutingFrame> own;
    for (auto& frame : monitor_->collect(timeout_ms, group, expected)) {
      if (frame.source_ip == "127.0.0.1" && frame.source_port == port_) {
        own.push_back(std::move(frame));
      }
    }
    return own;
  }

  [[nodiscard]] std::future<FcgiResponse> async_read(const std::string& query) const {
    const std::string socket = backend_->socket_path();
    return std::async(std::launch::async, [socket, query] {
      return cvknx::blackbox::fcgi_request(socket, "/r?" + cvknx::blackbox::with_session(query));
    });
  }

  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  std::string group_;
  std::uint16_t port_ = 0;
  std::unique_ptr<Knxd> knxd_;
  std::unique_ptr<Backend> backend_;
  std::unique_ptr<RoutingMonitor> monitor_;
  // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// ===========================================================================
// Protocol basics over the routing transport
// ===========================================================================

TEST_F(IpRoutingBlackBox, LoginAnswersTheCometVisuProtocolVersion) {
  const auto response = cvknx::blackbox::fcgi_request(backend_->socket_path(), "/l", "POST");
  EXPECT_EQ(response.status, 200) << backend_->log();
  EXPECT_NE(response.body.find("\"v\":\"0.0.2\""), std::string::npos) << response.body;
  EXPECT_NE(response.body.find("\"s\":\"0\""), std::string::npos) << response.body;
}

TEST_F(IpRoutingBlackBox, UnknownEndpointIs404) {
  const auto response = cvknx::blackbox::fcgi_request(backend_->socket_path(), "/nonexistent");
  EXPECT_EQ(response.status, 404);
  EXPECT_NE(response.body.find("unknown endpoint"), std::string::npos);
}

TEST_F(IpRoutingBlackBox, CacheOnlyReadWithoutDataIsEmpty) {
  const auto response = cache_only(1);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}") << response.body;
  EXPECT_EQ(cvknx::blackbox::response_position(response), 0U);
}

TEST_F(IpRoutingBlackBox, InvalidSessionIsRejected) {
  const auto response = read("a=" + group_text(1) + "&t=-1&s=not-a-session");
  EXPECT_EQ(response.status, 401);
  EXPECT_NE(response.body.find("invalid session"), std::string::npos);
}

TEST_F(IpRoutingBlackBox, InvalidAddressIsReportedAs404) {
  const auto response = read("a=not-an-address&t=-1");
  EXPECT_EQ(response.status, 404);
}

// ===========================================================================
// Bus -> backend: knxd publishes, the FastCGI client must see it
// ===========================================================================

TEST_F(IpRoutingBlackBox, KnxtoolTelegramReachesTheCacheOnlyRead) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(2), 3));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  const auto response = cache_only(2);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/2\":\"03\"}") << response.body;
  EXPECT_GE(cvknx::blackbox::response_position(response), 1U);
}

TEST_F(IpRoutingBlackBox, KnxtoolTelegramWakesABlockedLongPoll) {
  const std::string target = "a=" + group_text(3);
  auto pending = async_read(target);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout)
      << "the long-poll returned before any telegram was injected";

  const auto injected_at = Clock::now();
  ASSERT_TRUE(knxd_->groupswrite(group_text(3), 2));

  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "the long-poll did not receive the injected telegram";
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - injected_at);
  const auto response = pending.get();
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/3\":\"02\"}") << response.body;
  EXPECT_LT(elapsed.count(), 1500) << "delivery took " << elapsed.count() << " ms";
}

TEST_F(IpRoutingBlackBox, RawRoutingIndicationIsDecoded) {
  // Exactly what a foreign KNXnet/IP router puts on the backbone: service
  // 0x0530, cEMI L_Data.ind, APDU 00 80 42.
  ASSERT_TRUE(monitor_->inject_write(group_value(4), {0x00, 0x80, 0x42}, 0x1102));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(4);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/4\":\"42\"}") << response.body;
}

TEST_F(IpRoutingBlackBox, MultiByteValueIsDecoded) {
  ASSERT_TRUE(monitor_->inject_write(group_value(5), {0x00, 0x80, 0x0C, 0x6F}, 0x1102));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(5);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/5\":\"0c6f\"}")
      << response.body;
}

TEST_F(IpRoutingBlackBox, ResponseTelegramIsDecodedLikeAWrite) {
  // A_GroupValue_Response (0x40) carries a device answer, not a write.
  ASSERT_TRUE(monitor_->inject_write(group_value(6), {0x00, 0x40, 0x1F}, 0x1102));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(6);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/6\":\"1f\"}") << response.body;
}

TEST_F(IpRoutingBlackBox, GroupValueReadIsNotStoredAsAValue) {
  // 0x00 0x00 is A_GroupValue_Read: it must never be cached as a value.
  ASSERT_TRUE(monitor_->inject_write(group_value(7), {0x00, 0x00}, 0x1102));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(7);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}") << response.body;
}

TEST_F(IpRoutingBlackBox, LDataRequestEchoIsNotReportedAsBusTraffic) {
  // L_Data.req (0x11) is the confirmation of *our own* write for a router; a
  // telegram that only exists as .req must not appear as a fresh bus value.
  Bytes cemi = cvknx::blackbox::cemi_group_request(0x1102, group_value(8), {0x00, 0x80, 0x11});
  ASSERT_TRUE(monitor_->inject(cvknx::blackbox::routing_datagram(cemi, kRoutingIndicationService)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(8);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}") << response.body;
}

TEST_F(IpRoutingBlackBox, UnknownServiceTypeIsIgnored) {
  // A SEARCH_REQUEST must not be mistaken for bus traffic.
  Bytes cemi = cvknx::blackbox::cemi_group_indication(0x1102, group_value(9), {0x00, 0x80, 0x11});
  ASSERT_TRUE(monitor_->inject(cvknx::blackbox::routing_datagram(cemi, 0x0201)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(cvknx::blackbox::response_data_object(cache_only(9)), "{}");
}

TEST_F(IpRoutingBlackBox, LegacyRoutingServiceCodeIsIgnored) {
  // Regression guard: 0x0330 is not ROUTING_INDICATION (the correct code is
  // 0x0530).  A frame with the wrong code must be dropped, not decoded.
  Bytes cemi = cvknx::blackbox::cemi_group_indication(0x1102, group_value(10), {0x00, 0x80, 0x11});
  ASSERT_TRUE(monitor_->inject(cvknx::blackbox::routing_datagram(cemi, 0x0330)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(cvknx::blackbox::response_data_object(cache_only(10)), "{}");
}

TEST_F(IpRoutingBlackBox, IndividualTelegramIsIgnored) {
  // Destination is a physical address (control field 2 without the group bit).
  Bytes cemi = cvknx::blackbox::cemi_group_indication(0x1102, group_value(11), {0x00, 0x80, 0x11});
  cemi[3] = 0x60;  // no group bit
  ASSERT_TRUE(monitor_->inject(cvknx::blackbox::routing_datagram(cemi)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(cvknx::blackbox::response_data_object(cache_only(11)), "{}");
}

TEST_F(IpRoutingBlackBox, TruncatedFrameIsIgnored) {
  Bytes cemi = cvknx::blackbox::cemi_group_indication(0x1102, group_value(12), {0x00, 0x80, 0x11});
  cemi.resize(cemi.size() - 1);  // length byte no longer matches
  ASSERT_TRUE(monitor_->inject(cvknx::blackbox::routing_datagram(cemi)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(cvknx::blackbox::response_data_object(cache_only(12)), "{}");
}

TEST_F(IpRoutingBlackBox, LongPollIgnoresOtherAddresses) {
  auto pending = async_read("a=" + group_text(13));
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  ASSERT_TRUE(knxd_->groupswrite(group_text(14), 1));
  ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(400)), std::future_status::timeout)
      << "a telegram for another address must not end the long-poll";

  ASSERT_TRUE(knxd_->groupswrite(group_text(13), 5));
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = pending.get();
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/13\":\"05\"}")
      << response.body;
}

TEST_F(IpRoutingBlackBox, TwoIdenticalTelegramsAreDeliveredTwice) {
  // A repeated toggle is two bus events; deduplicating them would swallow a
  // second key press (AGENTS.md rule 1 is about duplicates, not repeats).
  const std::string target = "a=" + group_text(15);
  auto first = async_read(target);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(knxd_->groupswrite(group_text(15), 1));
  ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto first_response = first.get();

  auto second = async_read(
      target + "&i=" + std::to_string(cvknx::blackbox::response_position(first_response)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(knxd_->groupswrite(group_text(15), 1));
  ASSERT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "the identical second telegram must be reported again";
  const auto second_response = second.get();
  EXPECT_EQ(cvknx::blackbox::response_data_object(second_response), "{\"1/2/15\":\"01\"}")
      << second_response.body;
  EXPECT_GT(cvknx::blackbox::response_position(second_response),
            cvknx::blackbox::response_position(first_response));
}

TEST_F(IpRoutingBlackBox, PositionSkipsAlreadyDeliveredTelegrams) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(16), 1));
  const auto first = wait_for_value(16, "{\"1/2/16\":\"01\"}");
  const auto position = cvknx::blackbox::response_position(first);
  ASSERT_GT(position, 0U);

  const auto second = read("a=" + group_text(16) + "&t=1&i=" + std::to_string(position));
  EXPECT_EQ(cvknx::blackbox::response_data_object(second), "{}") << second.body;
}

TEST_F(IpRoutingBlackBox, MultipleAddressesInOneRequestAreReported) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(17), 1));
  ASSERT_TRUE(knxd_->groupswrite(group_text(18), 2));

  const auto response = wait_for_data("a=" + group_text(17) + "&a=" + group_text(18) + "&t=-1",
                                      "{\"1/2/17\":\"01\",\"1/2/18\":\"02\"}");
  EXPECT_EQ(cvknx::blackbox::response_data_object(response),
            "{\"1/2/17\":\"01\",\"1/2/18\":\"02\"}")
      << response.body;
}

TEST_F(IpRoutingBlackBox, LongPollTimesOutWithAnEmptyAnswer) {
  const auto started = Clock::now();
  const auto response = read("a=" + group_text(19) + "&t=1");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}");
  EXPECT_GE(elapsed.count(), 800) << "the poll returned too early";
}

TEST_F(IpRoutingBlackBox, LongestValueIsDecoded) {
  // 14 APDU bytes is the maximum a standard frame can carry.
  const Bytes apdu{0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05,
                   0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
  ASSERT_TRUE(monitor_->inject_write(group_value(40), apdu, 0x1102));

  const auto response = wait_for_value(40, "{\"1/2/40\":\"0102030405060708090a0b0c\"}");
  EXPECT_EQ(cvknx::blackbox::response_data_object(response),
            "{\"1/2/40\":\"0102030405060708090a0b0c\"}")
      << response.body;
}

TEST_F(IpRoutingBlackBox, WriteWithASessionIsAccepted) {
  const auto login =
      cvknx::blackbox::fcgi_request(backend_->socket_path(), "/l", "POST", "u=user&p=pass");
  ASSERT_EQ(login.status, 200) << login.body;
  const std::size_t start = login.body.find("\"s\":\"");
  ASSERT_NE(start, std::string::npos) << login.body;
  const std::size_t value_start = start + 5;
  const std::size_t value_end = login.body.find('"', value_start);
  ASSERT_NE(value_end, std::string::npos);
  const std::string session = login.body.substr(value_start, value_end - value_start);

  monitor_->drain();
  const auto response = cvknx::blackbox::fcgi_request(
      backend_->socket_path(), "/w?a=" + group_text(41) + "&v=8001&s=" + session);
  EXPECT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(backend_frames(group_value(41), 1500, 1).size(), 1U);
}

// ===========================================================================
// Backend -> bus: what the backend puts on the wire
// ===========================================================================

TEST_F(IpRoutingBlackBox, WriteIsPublishedAsRoutingIndication) {
  monitor_->drain();
  const auto response = write(group_text(20), "8042");
  EXPECT_EQ(response.status, 200) << response.body;

  const auto frames = backend_frames(group_value(20), 1500, 1);
  ASSERT_EQ(frames.size(), 1U) << "exactly one ROUTING_INDICATION expected, got " << frames.size()
                               << "\n"
                               << backend_->log();
  const auto& frame = frames.front();
  EXPECT_EQ(frame.service, kRoutingIndicationService)
      << "ROUTING_INDICATION is 0x0530, got service 0x" << std::hex << frame.service
      << " (cEMI leader 0x" << static_cast<int>(frame.cemi[0]) << ")";

  // cEMI: [0]=0x29 L_Data.ind, [2]=0xBC, [3]=0xE0, [4..5]=source, [6..7]=group.
  ASSERT_GE(frame.cemi.size(), 9U);
  EXPECT_EQ(frame.cemi[0], 0x29) << "routing is a bus *indication*; 0x11 is ignored by routers";
  EXPECT_EQ(frame.cemi[3], 0xE0) << "group destination flag";
  const auto source = static_cast<std::uint16_t>((frame.cemi[4] << 8) | frame.cemi[5]);
  EXPECT_EQ(source, kDefaultSourceIa) << "the routing individual address";
  const auto destination = static_cast<std::uint16_t>((frame.cemi[6] << 8) | frame.cemi[7]);
  EXPECT_EQ(destination, group_value(20));
  const Bytes apdu(frame.cemi.begin() + 9, frame.cemi.end());
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x42}));
  EXPECT_EQ(frame.ttl, 16) << "KNXnet/IP routing uses TTL 16";
}

TEST_F(IpRoutingBlackBox, WriteIsPublishedExactlyOnce) {
  monitor_->drain();
  ASSERT_EQ(write(group_text(21), "807e").status, 200);
  const auto frames = backend_frames(group_value(21), 700);
  EXPECT_EQ(frames.size(), 1U) << "a single write must not be announced twice";
}

TEST_F(IpRoutingBlackBox, MultiByteWriteIsPublished) {
  monitor_->drain();
  ASSERT_EQ(write(group_text(22), "800c6f").status, 200);
  const auto frames = backend_frames(group_value(22), 1500, 1);
  ASSERT_EQ(frames.size(), 1U);
  const Bytes apdu(frames.front().cemi.begin() + 9, frames.front().cemi.end());
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));
}

TEST_F(IpRoutingBlackBox, WritingTwoAddressesPublishesBothTelegrams) {
  monitor_->drain();
  const auto response = cvknx::blackbox::fcgi_request(
      backend_->socket_path(), "/w?a=" + group_text(23) + "&a=" + group_text(24) + "&v=8001&s=0");
  EXPECT_EQ(response.status, 200);

  const auto first = backend_frames(group_value(23), 1500, 1);
  const auto second = backend_frames(group_value(24), 1500, 1);
  EXPECT_EQ(first.size(), 1U);
  EXPECT_EQ(second.size(), 1U);
}

TEST_F(IpRoutingBlackBox, WriteReachesTheKnxdBus) {
  // The independent witness: knxd accepted the telegram and cached it.
  ASSERT_EQ(write(group_text(25), "8042").status, 200);
  std::string cached;
  for (int attempt = 0; attempt < 40; ++attempt) {
    cached = knxd_->groupcache_read(group_text(25));
    if (!cached.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_EQ(cached, "42") << "knxd never saw the write\n"
                          << backend_->log() << "\n"
                          << knxd_->log();
}

TEST_F(IpRoutingBlackBox, WriteSourceAddressIsTheConfiguredIndividualAddress) {
  StartBackend(20, {}, "1.1.240");
  monitor_->drain();
  ASSERT_EQ(write(group_text(26), "8001").status, 200);
  const auto frames = backend_frames(group_value(26), 1500, 1);
  ASSERT_EQ(frames.size(), 1U);
  const auto source =
      static_cast<std::uint16_t>((frames.front().cemi[4] << 8) | frames.front().cemi[5]);
  EXPECT_EQ(source, 0x11F0) << "KNX_IP_INDIVIDUAL_ADDR must be used as the cEMI source";

  std::string cached = knxd_->groupcache_read(group_text(26));
  EXPECT_EQ(cached, "01") << knxd_->log();
}

TEST_F(IpRoutingBlackBox, InvalidValueIsRejectedAndPublishesNothing) {
  monitor_->drain();
  const auto response = write(group_text(27), "zz");
  EXPECT_EQ(response.status, 400) << "an unusable value is a client error";
  EXPECT_TRUE(backend_frames(group_value(27), 500).empty());
}

TEST_F(IpRoutingBlackBox, InvalidAddressIsRejectedWithoutSending) {
  monitor_->drain();
  const auto response =
      cvknx::blackbox::fcgi_request(backend_->socket_path(), "/w?a=not-an-address&v=8001&s=0");
  EXPECT_EQ(response.status, 404);
  EXPECT_TRUE(backend_frames(group_value(28), 400).empty());
}

TEST_F(IpRoutingBlackBox, InvalidSessionDoesNotPublish) {
  monitor_->drain();
  const auto response = cvknx::blackbox::fcgi_request(backend_->socket_path(),
                                                      "/w?a=" + group_text(29) + "&v=8001&s=bogus");
  EXPECT_EQ(response.status, 401);
  EXPECT_TRUE(backend_frames(group_value(29), 400).empty());
}

TEST_F(IpRoutingBlackBox, GroupValueReadRequestsArePublished) {
  // `t=0` on an uncached address asks the bus — a routing participant sends a
  // GroupValueRead instead of a write.
  monitor_->drain();
  const auto response = read("a=" + group_text(30) + "&t=0");
  EXPECT_EQ(response.status, 200);

  const auto frames = backend_frames(group_value(30), 1500, 1);
  ASSERT_EQ(frames.size(), 1U) << backend_->log();
  const Bytes apdu(frames.front().cemi.begin() + 9, frames.front().cemi.end());
  EXPECT_EQ(apdu, (Bytes{0x00, 0x00})) << "A_GroupValue_Read";
}

// ===========================================================================
// Own echo / multi client semantics (AGENTS.md rule 1)
// ===========================================================================

TEST_F(IpRoutingBlackBox, WriteIsDeliveredToTheBlockedClientExactlyOnce) {
  const std::string target = "a=" + group_text(31);
  auto pending = async_read(target);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  ASSERT_EQ(write(group_text(31), "8042").status, 200);
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = pending.get();
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/31\":\"42\"}")
      << response.body;

  // The next poll must not repeat the same value ("i" from the response).
  const auto next =
      read(target + "&i=" + std::to_string(cvknx::blackbox::response_position(response)) + "&t=1");
  EXPECT_EQ(cvknx::blackbox::response_data_object(next), "{}")
      << "the write was reported twice: " << next.body;
}

TEST_F(IpRoutingBlackBox, WriteFromASecondClientWakesTheFirst) {
  const std::string first_client = backend_->socket_path();
  auto pending = std::async(std::launch::async, [first_client, this] {
    return cvknx::blackbox::fcgi_request(first_client, "/r?a=" + group_text(32) + "&s=0");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // A second FastCGI connection writes the value.
  const auto write_response = cvknx::blackbox::fcgi_request(
      backend_->socket_path(), "/w?a=" + group_text(32) + "&v=8002&s=0");
  ASSERT_EQ(write_response.status, 200);

  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_EQ(cvknx::blackbox::response_data_object(pending.get()), "{\"1/2/32\":\"02\"}");
}

TEST_F(IpRoutingBlackBox, BlockedLongPollDoesNotDelayAWrite) {
  auto pending = async_read("a=" + group_text(33) + "&t=3");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  const auto started = Clock::now();
  const auto response = write(group_text(34), "8001");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  EXPECT_EQ(response.status, 200);
  EXPECT_LT(elapsed.count(), 1500)
      << "the write waited for the long-poll: " << elapsed.count() << " ms";

  // The long-poll for the *other* address is still blocked, and it ends through
  // its own timeout.
  ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(cvknx::blackbox::response_data_object(pending.get()), "{}");
}

TEST_F(IpRoutingBlackBox, EveryBlockedClientReceivesTheSameTelegram) {
  constexpr int kClients = 6;
  std::vector<std::future<FcgiResponse>> clients;
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(async_read("a=" + group_text(35)));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ASSERT_TRUE(knxd_->groupswrite(group_text(35), 7));

  for (auto& client : clients) {
    ASSERT_EQ(client.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const auto response = client.get();
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/35\":\"07\"}");
  }
}

// ===========================================================================
// Stateless transport: no invented outage
// ===========================================================================

TEST_F(IpRoutingBlackBox, RoutingDoesNotInventABusOutage) {
  // IP routing is stateless: no ACK, no connection.  knxd disappearing cannot
  // be detected, so the response must not claim an outage block.
  knxd_->stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto response = cache_only(36);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body.find("\"bus\""), std::string::npos) << response.body;

  monitor_->drain();
  const auto write_response = write(group_text(36), "8001");
  EXPECT_EQ(write_response.status, 200) << write_response.body;
  EXPECT_EQ(backend_frames(group_value(36), 1500, 1).size(), 1U);
  EXPECT_TRUE(backend_->running()) << "a dead router must not kill the backend";
}

TEST_F(IpRoutingBlackBox, BackendRestartKeepsServingTheBus) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(37), 4));
  ASSERT_EQ(cvknx::blackbox::response_data_object(wait_for_value(37, "{\"1/2/37\":\"04\"}")),
            "{\"1/2/37\":\"04\"}");

  StartBackend();
  ASSERT_TRUE(knxd_->groupswrite(group_text(37), 5));
  EXPECT_EQ(cvknx::blackbox::response_data_object(wait_for_value(37, "{\"1/2/37\":\"05\"}")),
            "{\"1/2/37\":\"05\"}")
      << backend_->log();
}

}  // namespace
