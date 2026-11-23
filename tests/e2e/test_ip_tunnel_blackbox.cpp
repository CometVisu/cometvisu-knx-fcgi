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
 * @file test_ip_tunnel_blackbox.cpp
 * @brief Black-box tests of `KNX_TRANSPORT=ip-tunnel` against a real knxd.
 *
 * Topology of every test:
 *
 * @code
 *   knxtool groupswrite ──► knxd ──► tunnel server ──unicast──► backend ──► /r
 *                              ▲                                   │
 *                              └──────── TUNNELLING_REQUEST ◄──────┘ (/w)
 * @endcode
 *
 * knxd runs as a KNXnet/IP *tunnel server* (`-T -S group:port`), which is what
 * an MDT/Gira/Siemens interface does in the field: CONNECT/CONNECTIONSTATE/
 * DISCONNECT, one channel, sequence numbers and ACKs.  `knxtool` is the
 * independent witness that a write really arrived on the bus.
 *
 * A second backend instance is used where a test needs a second KNXnet/IP
 * client (multi-client delivery, "another tab sees my write").
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
using cvknx::blackbox::FcgiResponse;
using cvknx::blackbox::Knxd;
using cvknx::blackbox::KnxdMode;
using cvknx::blackbox::KnxdOptions;
using Clock = std::chrono::steady_clock;

class IpTunnelBlackBox : public ::testing::Test {
protected:
  void SetUp() override {
    if (!cvknx::blackbox::knxd_available()) {
      GTEST_SKIP() << "knxd/knxtool are not installed";
    }
    group_ = cvknx::blackbox::unique_multicast_group();
    port_ = cvknx::blackbox::free_udp_port();
    ASSERT_NE(port_, 0) << "no free UDP port";

    KnxdOptions knxd_options;
    knxd_options.mode = KnxdMode::TunnelOnly;
    knxd_options.group = group_;
    knxd_options.tunnel_port = port_;
    knxd_ = std::make_unique<Knxd>(knxd_options);
    ASSERT_TRUE(knxd_->start()) << knxd_->log();

    backend_ = StartBackend();
    ASSERT_NE(backend_, nullptr);
  }

  void TearDown() override {
    if (second_) {
      second_->stop();
    }
    if (backend_) {
      backend_->stop();
    }
    if (knxd_) {
      knxd_->stop();
    }
  }

  [[nodiscard]] std::unique_ptr<Backend> StartBackend(const cvknx::blackbox::Env& extra = {},
                                                      int longpoll_seconds = 20) const {
    BackendOptions options;
    // Several workers: a blocked long-poll must never keep /w from being
    // served (production defaults to 20).  Two of them are reserved for the
    // short requests, so 8 workers leave 6 slots for concurrent long-polls.
    options.workers = 8;
    options.longpoll_seconds = longpoll_seconds;
    options.env = {{"KNX_TRANSPORT", "ip-tunnel"},
                   {"KNX_IP_HOST", "127.0.0.1"},
                   {"KNX_IP_PORT", std::to_string(port_)},
                   // Keep failure detection fast: one missed heartbeat ends
                   // the tunnel within a heartbeat interval plus one timeout.
                   {"KNX_IP_HEARTBEAT_SEC", "10"},
                   {"KNX_IP_ACK_TIMEOUT_MS", "300"},
                   {"KNX_IP_RETRIES", "2"}};
    for (const auto& entry : extra) {
      options.env.push_back(entry);
    }
    auto backend = std::make_unique<Backend>(options);
    EXPECT_TRUE(backend->start()) << backend->log();
    return backend;
  }

  [[nodiscard]] std::string group_text(int sub) const { return "1/2/" + std::to_string(sub); }

  [[nodiscard]] FcgiResponse read(const Backend& backend, const std::string& query) const {
    return cvknx::blackbox::fcgi_request(backend.socket_path(),
                                         "/r?" + cvknx::blackbox::with_session(query));
  }
  [[nodiscard]] FcgiResponse cache_only(const Backend& backend, int sub) const {
    return read(backend, "a=" + group_text(sub) + "&t=-1");
  }

  /// Poll a cache-only read until the expected data object appears: a telegram
  /// injected through knxd needs a moment to reach the tunnel.
  [[nodiscard]] FcgiResponse wait_for_value(const Backend& backend, int sub,
                                            const std::string& expected,
                                            int timeout_ms = 3000) const {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    FcgiResponse response = cache_only(backend, sub);
    while (Clock::now() < deadline) {
      if (cvknx::blackbox::response_data_object(response) == expected) {
        return response;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      response = cache_only(backend, sub);
    }
    return response;
  }
  [[nodiscard]] FcgiResponse write(const Backend& backend, const std::string& group,
                                   const std::string& value) const {
    return cvknx::blackbox::fcgi_request(
        backend.socket_path(), "/w?" + cvknx::blackbox::with_session("a=" + group + "&v=" + value));
  }

  [[nodiscard]] bool has_bus_outage(const FcgiResponse& response) const {
    return response.body.find("\"bus\"") != std::string::npos &&
           response.body.find("\"ok\":false") != std::string::npos;
  }

  /// Poll the group cache until knxd knows the value or the deadline expires.
  [[nodiscard]] std::string wait_for_cache(const std::string& group, int timeout_ms = 5000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string cached;
    while (Clock::now() < deadline) {
      cached = knxd_->groupcache_read(group);
      if (!cached.empty()) {
        return cached;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cached;
  }

  /// Wait until the tunnel really works again.
  ///
  /// The bus status alone cannot prove it: a stale channel (the device forgot
  /// it) still reports "available" until the heartbeat notices.  The only
  /// reliable witness is a telegram that actually reaches knxd's bus.
  [[nodiscard]] bool wait_for_reconnect(int timeout_ms = 45000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int value = 1;
    while (Clock::now() < deadline) {
      const std::string group = group_text(91);
      const auto response =
          write(*backend_, group, "800" + std::to_string(value % 8));  // 8000..8007
      if (response.status == 200 && !wait_for_cache(group, 800).empty()) {
        return true;
      }
      ++value;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
  }

  /// Wait until the backend reports the bus as unavailable.
  /// A missed heartbeat is only detected when it is due (KNX_IP_HEARTBEAT_SEC,
  /// minimum 10 s) plus the state timeout — hence the generous window.
  [[nodiscard]] bool wait_for_outage(const Backend& backend, int timeout_ms = 35000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
      const auto response = cache_only(backend, 98);
      if (response.status == 503 && has_bus_outage(response)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  std::string group_;
  std::uint16_t port_ = 0;
  std::unique_ptr<Knxd> knxd_;
  std::unique_ptr<Backend> backend_;
  std::unique_ptr<Backend> second_;
  // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// ===========================================================================
// Protocol basics over a tunnelling connection
// ===========================================================================

TEST_F(IpTunnelBlackBox, LoginAnswersTheCometVisuProtocolVersion) {
  const auto response = cvknx::blackbox::fcgi_request(backend_->socket_path(), "/l", "POST");
  EXPECT_EQ(response.status, 200) << backend_->log();
  EXPECT_NE(response.body.find("\"v\":\"0.0.2\""), std::string::npos) << response.body;
  EXPECT_NE(response.body.find("\"s\":\"0\""), std::string::npos) << response.body;
}

TEST_F(IpTunnelBlackBox, TunnelIsEstablishedWithoutAnOutageBlock) {
  // A healthy tunnel must not report a bus outage; the tunnel is up even
  // though the cache is empty.
  const auto response = cache_only(*backend_, 1);
  EXPECT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}") << response.body;
  EXPECT_FALSE(has_bus_outage(response)) << response.body;
  EXPECT_TRUE(backend_->log().find("Tunnel established") != std::string::npos) << backend_->log();
}

TEST_F(IpTunnelBlackBox, InvalidSessionIsRejected) {
  const auto response = read(*backend_, "a=" + group_text(2) + "&t=-1&s=bogus");
  EXPECT_EQ(response.status, 401);
}

// ===========================================================================
// Bus -> backend
// ===========================================================================

TEST_F(IpTunnelBlackBox, KnxtoolTelegramReachesTheCacheOnlyRead) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(3), 3));
  const auto response = wait_for_value(*backend_, 3, "{\"1/2/3\":\"03\"}");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/3\":\"03\"}")
      << response.body << "\n"
      << backend_->log();
}

TEST_F(IpTunnelBlackBox, KnxtoolTelegramWakesABlockedLongPoll) {
  const std::string socket = backend_->socket_path();
  auto pending = std::async(std::launch::async, [socket, this] {
    return cvknx::blackbox::fcgi_request(socket, "/r?a=" + group_text(4) + "&s=0");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

  const auto injected_at = Clock::now();
  ASSERT_TRUE(knxd_->groupswrite(group_text(4), 5));
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "the tunnel did not deliver the injected telegram";
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - injected_at);

  const auto response = pending.get();
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/4\":\"05\"}") << response.body;
  EXPECT_LT(elapsed.count(), 1500) << "delivery took " << elapsed.count() << " ms";
}

TEST_F(IpTunnelBlackBox, MultiByteValueIsDecoded) {
  ASSERT_TRUE(knxd_->groupswrite(group_text(5), 2));
  const auto response = wait_for_value(*backend_, 5, "{\"1/2/5\":\"02\"}");
  ASSERT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/5\":\"02\"}") << response.body;
}

// ===========================================================================
// Backend -> bus (the tunnelling client must be accepted by the device)
// ===========================================================================

TEST_F(IpTunnelBlackBox, WriteReachesTheKnxdBus) {
  ASSERT_EQ(write(*backend_, group_text(6), "8042").status, 200);
  EXPECT_EQ(wait_for_cache(group_text(6)), "42") << "knxd never received the tunnel telegram\n"
                                                 << backend_->log() << "\n"
                                                 << knxd_->log();
}

TEST_F(IpTunnelBlackBox, MultiByteWriteReachesTheKnxdBus) {
  ASSERT_EQ(write(*backend_, group_text(7), "800c6f").status, 200);
  EXPECT_EQ(wait_for_cache(group_text(7)), "0c6f") << backend_->log();
}

TEST_F(IpTunnelBlackBox, SequentialWritesAreAllDelivered) {
  for (int value = 1; value <= 6; ++value) {
    ASSERT_EQ(write(*backend_, group_text(8), "800" + std::to_string(value)).status, 200)
        << "write " << value << " failed";
  }
  // The writes are paced by the global rate limit (10 telegrams/s), so they
  // reach the bus over roughly half a second.  Wait for the *last* value: the
  // earlier ones are visible on the way there.
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  std::string cached;
  while (Clock::now() < deadline) {
    cached = knxd_->groupcache_read(group_text(8));
    if (cached == "06") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_EQ(cached, "06") << "the last of six writes must be the cached value\n" << backend_->log();
}

TEST_F(IpTunnelBlackBox, WriteSourceAddressIsTheDeviceAssignedAddress) {
  // knxd assigns 1.1.221 to its first tunnel client (CRD); the cEMI source
  // must be that address, not 0.0.0.
  ASSERT_EQ(write(*backend_, group_text(9), "8001").status, 200);
  ASSERT_EQ(wait_for_cache(group_text(9)), "01");
  EXPECT_TRUE(backend_->log().find("source 1.1.221") != std::string::npos)
      << "the tunnel must use the CRD address as its source\n"
      << backend_->log();
}

TEST_F(IpTunnelBlackBox, InvalidValueIsRejectedAndPublishesNothing) {
  const auto response = write(*backend_, group_text(10), "zz");
  // An unusable value is a client error, not a silent success.
  EXPECT_EQ(response.status, 400);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(knxd_->groupcache_read(group_text(10)).empty());
}

// ===========================================================================
// Multi client: two tunnels on one device
// ===========================================================================

TEST_F(IpTunnelBlackBox, SecondBackendSeesTheFirstWrites) {
  second_ = StartBackend();
  ASSERT_NE(second_, nullptr) << "a second tunnel must be accepted by the device";
  ASSERT_TRUE(second_->wait_for_log("Tunnel established")) << second_->log();

  const std::string socket = second_->socket_path();
  auto pending = std::async(std::launch::async, [socket, this] {
    return cvknx::blackbox::fcgi_request(socket, "/r?a=" + group_text(11) + "&s=0");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  ASSERT_EQ(write(*backend_, group_text(11), "8042").status, 200);
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "the telegram of the first tunnel never reached the second one";
  const auto response = pending.get();
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/11\":\"42\"}")
      << response.body;
}

TEST_F(IpTunnelBlackBox, OwnWriteIsNotEchoedToTheWriter) {
  // A tunnelling interface confirms the frame with L_Data.con and does not
  // route it back to the tunnel that sent it.  The value therefore appears in
  // knxd's cache and in the *other* tunnel (see SecondBackendSeesTheFirstWrites),
  // but never as a second inbound telegram for the writer.
  ASSERT_EQ(write(*backend_, group_text(12), "8042").status, 200);
  ASSERT_EQ(wait_for_cache(group_text(12)), "42") << backend_->log();

  const auto response = read(*backend_, "a=" + group_text(12) + "&t=1");
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}")
      << "the write came back as an inbound telegram: " << response.body;
}

TEST_F(IpTunnelBlackBox, MultiAddressWriteReachesEveryAddress) {
  const auto response = cvknx::blackbox::fcgi_request(
      backend_->socket_path(), "/w?a=" + group_text(21) + "&a=" + group_text(22) + "&v=800c6f&s=0");
  ASSERT_EQ(response.status, 200) << response.body;
  // Each telegram is sent with its own sequence number; both must be ACKed
  // and reach the bus.
  EXPECT_EQ(wait_for_cache(group_text(21)), "0c6f") << backend_->log();
  EXPECT_EQ(wait_for_cache(group_text(22)), "0c6f") << backend_->log();
}

TEST_F(IpTunnelBlackBox, LongPollTimesOutWithAnEmptyAnswer) {
  const auto started = Clock::now();
  const auto response = read(*backend_, "a=" + group_text(23) + "&t=1");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{}");
  EXPECT_GE(elapsed.count(), 800) << "the poll returned too early";
}

TEST_F(IpTunnelBlackBox, BlockedLongPollDoesNotDelayAWrite) {
  const std::string socket = backend_->socket_path();
  auto pending = std::async(std::launch::async, [socket, this] {
    return cvknx::blackbox::fcgi_request(socket, "/r?a=" + group_text(13) + "&t=3&s=0");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const auto started = Clock::now();
  const auto response = write(*backend_, group_text(14), "8001");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  EXPECT_EQ(response.status, 200);
  EXPECT_LT(elapsed.count(), 1500) << "the write waited " << elapsed.count() << " ms";

  ASSERT_EQ(pending.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

// ===========================================================================
// Outage handling and recovery (AGENTS.md decision 9)
// ===========================================================================

TEST_F(IpTunnelBlackBox, DeadDeviceIsReportedAndReconnected) {
  ASSERT_EQ(write(*backend_, group_text(15), "8001").status, 200);
  ASSERT_EQ(wait_for_cache(group_text(15)), "01");

  // The device forgets our channel: the bus is gone.
  knxd_->stop();
  ASSERT_TRUE(wait_for_outage(*backend_)) << "the backend never reported the outage\n"
                                          << backend_->log();

  // A write during the outage is rejected and must not be queued.
  const auto rejected = write(*backend_, group_text(16), "8002");
  EXPECT_EQ(rejected.status, 503) << rejected.body;
  EXPECT_NE(rejected.body.find("write failed"), std::string::npos) << rejected.body;
  EXPECT_TRUE(has_bus_outage(rejected)) << rejected.body;

  // The device comes back: the reader reconnects on its own.
  ASSERT_TRUE(knxd_->restart()) << knxd_->log();
  ASSERT_TRUE(wait_for_reconnect()) << "the tunnel never reconnected\n" << backend_->log();

  EXPECT_TRUE(knxd_->groupcache_read(group_text(16)).empty())
      << "a write rejected during the outage must not be delivered later";

  ASSERT_EQ(write(*backend_, group_text(17), "8003").status, 200);
  EXPECT_EQ(wait_for_cache(group_text(17)), "03") << backend_->log();
}

TEST_F(IpTunnelBlackBox, InjectionAfterReconnectStillWakesTheLongPoll) {
  ASSERT_TRUE(knxd_->restart()) << knxd_->log();
  ASSERT_TRUE(wait_for_reconnect()) << backend_->log();

  const std::string socket = backend_->socket_path();
  auto pending = std::async(std::launch::async, [socket, this] {
    return cvknx::blackbox::fcgi_request(socket, "/r?a=" + group_text(18) + "&s=0");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_TRUE(knxd_->groupswrite(group_text(18), 6));
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_EQ(cvknx::blackbox::response_data_object(pending.get()), "{\"1/2/18\":\"06\"}");
}

// ===========================================================================
// Transport options
// ===========================================================================

TEST_F(IpTunnelBlackBox, NatModeConnects) {
  backend_->stop();
  backend_ = StartBackend({{"KNX_IP_NAT", "1"}});
  ASSERT_NE(backend_, nullptr);
  ASSERT_TRUE(backend_->wait_for_log("Tunnel established")) << backend_->log();

  ASSERT_EQ(write(*backend_, group_text(19), "8007").status, 200);
  EXPECT_EQ(wait_for_cache(group_text(19)), "07") << backend_->log();
}

TEST_F(IpTunnelBlackBox, ExplicitLocalAddressConnects) {
  backend_->stop();
  backend_ = StartBackend({{"KNX_IP_LOCAL_ADDR", "127.0.0.1"}});
  ASSERT_NE(backend_, nullptr);
  ASSERT_TRUE(backend_->wait_for_log("Tunnel established")) << backend_->log();

  ASSERT_TRUE(knxd_->groupswrite(group_text(20), 4));
  const auto response = wait_for_value(*backend_, 20, "{\"1/2/20\":\"04\"}");
  EXPECT_EQ(cvknx::blackbox::response_data_object(response), "{\"1/2/20\":\"04\"}")
      << backend_->log();
}

}  // namespace
