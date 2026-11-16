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
 * @file test_transport_recovery.cpp
 * @brief Real tunnelling link + shared TX queue against a rate-limited,
 *        restarting router.
 *
 * This is the closest thing to a real installation that runs in CI: the
 * production `TunnelingBusLink` and `run_cache_reader()` are driven by many
 * producer threads through the production `SharedTxQueue`, while a scriptable
 * UDP "router" answers slowly, stops answering, or drops the tunnel.
 *
 * Invariants under test:
 *   - every accepted write reaches the device exactly once, in order;
 *   - a silent device loses nothing (the queue keeps the telegrams);
 *   - a device that drops the tunnel is reconnected, and the backlog is then
 *     delivered on the new channel;
 *   - retransmissions (rate-limited bus) reuse the sequence number, so a real
 *     device deduplicates them.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/bus/cache_reader.h"
#include "../../src/knxnetip/cemi.h"
#include "../../src/knxnetip/knxnetip_frame.h"
#include "../../src/knxnetip/tunneling_bus_link.h"
#include "../../src/state/shared_group_cache.h"
#include "../../src/state/shared_tx_queue.h"

namespace cvknx {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using knxnetip::Hpai;
using knxnetip::ServiceType;

/// A KNXnet/IP device that can be slow, silent or forget its tunnel.
class ScriptableRouter {
public:
  ScriptableRouter() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)), 0);
    socklen_t len = sizeof(address);
    (void)::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&address), &len);
    port_ = ntohs(address.sin_port);
  }
  ~ScriptableRouter() {
    stop();
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  ScriptableRouter(const ScriptableRouter&) = delete;
  ScriptableRouter& operator=(const ScriptableRouter&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

  void set_ack_delay_ms(int delay) { ack_delay_ms_ = delay; }
  void set_silent(bool silent) { silent_ = silent; }
  /// The device forgets the tunnel and reports the channel as unknown.
  void drop_tunnel() {
    std::uint8_t channel = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      channel = channel_;
    }
    const auto response =
        knxnetip::build_frame(ServiceType::ConnectionStateResponse, {channel, 0x04});
    if (have_peer_) {
      (void)::sendto(fd_, response.data(), response.size(), 0,
                     reinterpret_cast<const struct sockaddr*>(&peer_), sizeof(peer_));
    }
  }

  void start() {
    running_ = true;
    thread_ = std::thread([this] { loop(); });
  }
  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Telegrams the "bus" received, deduplicated by sequence like a real device.
  [[nodiscard]] std::vector<std::pair<std::uint16_t, Bytes>> delivered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return delivered_;
  }
  [[nodiscard]] int connect_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connect_requests_;
  }
  [[nodiscard]] int channels_used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_used_;
  }
  [[nodiscard]] int retransmissions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retransmissions_;
  }

private:
  void loop() {
    while (running_) {
      struct pollfd pfd {};
      pfd.fd = fd_;
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, 2) <= 0) {
        continue;
      }
      std::uint8_t buffer[2048] = {};
      struct sockaddr_in from {};
      socklen_t from_len = sizeof(from);
      const ssize_t received = ::recvfrom(fd_, buffer, sizeof(buffer), 0,
                                          reinterpret_cast<struct sockaddr*>(&from), &from_len);
      if (received <= 0) {
        continue;
      }
      peer_ = from;
      have_peer_ = true;
      if (silent_) {
        continue;  // the device is gone: nothing is answered
      }
      const std::vector<std::uint8_t> datagram(buffer,
                                               buffer + static_cast<std::ptrdiff_t>(received));
      handle(datagram);
    }
  }

  void handle(const Bytes& datagram) {
    const auto frame = knxnetip::parse_frame(datagram);
    if (!frame) {
      return;
    }
    switch (frame->service) {
      case ServiceType::ConnectRequest: {
        std::lock_guard<std::mutex> lock(mutex_);
        ++connect_requests_;
        channel_ = static_cast<std::uint8_t>(0x20 + (connect_requests_ % 0x5F));
        ++channels_used_;
        seen_sequences_.clear();
        break;
      }
      case ServiceType::ConnectionStateRequest:
        reply(knxnetip::build_frame(ServiceType::ConnectionStateResponse, {channel_, 0x00}));
        return;
      case ServiceType::DisconnectRequest:
        reply(knxnetip::build_frame(ServiceType::DisconnectResponse, {channel_, 0x00}));
        return;
      case ServiceType::TunnellingRequest: {
        const auto request = knxnetip::parse_tunnelling_request(datagram);
        if (!request) {
          return;
        }
        if (ack_delay_ms_ > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(ack_delay_ms_));
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          // A real device ACKs a retransmission but delivers the telegram once.
          if (!seen_sequences_.insert(request->sequence).second) {
            ++retransmissions_;
          } else if (request->cemi.size() >= 9) {
            // The device gets L_Data.req (0x11) — parse destination + APDU.
            const auto& cemi_bytes = request->cemi;
            const auto destination = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(cemi_bytes[6]) << 8) | cemi_bytes[7]);
            Bytes apdu(cemi_bytes.begin() + 9, cemi_bytes.end());
            delivered_.emplace_back(destination, apdu);
          }
        }
        reply(knxnetip::build_tunnelling_ack(channel_, request->sequence));
        return;
      }
      default:
        return;
    }
    // answer a CONNECT_REQUEST
    const auto response_body = [this] {
      Bytes body{channel_, 0x00};
      const auto hpai = knxnetip::encode_hpai(Hpai{0x7F000001U, port_});
      body.insert(body.end(), hpai.begin(), hpai.end());
      body.push_back(0x04);
      body.push_back(0x04);
      body.push_back(0x11);
      body.push_back(0x01);
      return body;
    }();
    reply(knxnetip::build_frame(ServiceType::ConnectResponse, response_body));
  }

  void reply(const Bytes& data) {
    if (!have_peer_) {
      return;
    }
    (void)::sendto(fd_, data.data(), data.size(), 0,
                   reinterpret_cast<const struct sockaddr*>(&peer_), sizeof(peer_));
  }

  int fd_ = -1;
  std::uint16_t port_ = 0;
  struct sockaddr_in peer_ {};
  bool have_peer_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;

  mutable std::mutex mutex_;
  std::uint8_t channel_ = 0x21;
  int connect_requests_ = 0;
  int channels_used_ = 0;
  int retransmissions_ = 0;
  int ack_delay_ms_ = 0;
  std::atomic<bool> silent_{false};
  std::set<std::uint8_t> seen_sequences_;
  std::vector<std::pair<std::uint16_t, Bytes>> delivered_;
};

TunnelOptions RouterOptions(std::uint16_t port) {
  TunnelOptions options;
  options.host = "127.0.0.1";
  options.port = port;
  options.ack_timeout_ms = 60;
  options.retries = 3;
  options.heartbeat_sec = 60;
  options.connect_timeout_ms = 1000;
  return options;
}

CacheReaderOptions ReaderOptions() {
  CacheReaderOptions options;
  options.wait_timeout_ms = 5;
  options.reconnect_backoff_ms = 20;
  options.max_reconnect_backoff_ms = 40;
  options.sleep_slice_ms = 2;
  // Faster than the production 10/s: these tests assert throughput.
  options.max_tx_per_sec = 0;
  return options;
}

/// Telegram identity: producer in byte 2, sequence in bytes 3..4.
Bytes Payload(int producer, int sequence) {
  return {0x00, 0x80, static_cast<std::uint8_t>(producer),
          static_cast<std::uint8_t>((sequence >> 8) & 0xFF),
          static_cast<std::uint8_t>(sequence & 0xFF)};
}
std::uint16_t Address(int producer, int sequence) {
  return static_cast<std::uint16_t>((producer << 11) | (sequence & 0x07FF));
}

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

/// Thread wrapper for run_cache_reader(): always stops and joins, so a failed
/// assertion cannot terminate the test process.
class ReaderThread {
public:
  ReaderThread(TunnelingBusLink& link, SharedGroupCache& cache, SharedTxQueue& queue)
      : link_(link) {
    thread_ = std::thread([this, &cache, &queue] {
      run_cache_reader(link_, cache, &queue, ReaderOptions(), [this] { return stop_.load(); });
    });
  }
  ~ReaderThread() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
    link_.disconnect();
  }
  ReaderThread(const ReaderThread&) = delete;
  ReaderThread& operator=(const ReaderThread&) = delete;

private:
  TunnelingBusLink& link_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

class TransportRecoveryTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(cache_.create());
    ASSERT_TRUE(queue_.create());
    router_.start();
  }
  void TearDown() override { router_.stop(); }

  SharedGroupCache cache_;
  SharedTxQueue queue_;
  ScriptableRouter router_;

  /// Push telegrams from @p producers threads, retrying while the ring is full
  /// (a real worker would answer 503 — here the lossless path is under test).
  void PushConcurrently(int producers, int per_producer) {
    pushers_.clear();
    for (int p = 0; p < producers; ++p) {
      pushers_.emplace_back([this, p, per_producer] {
        for (int i = 0; i < per_producer; ++i) {
          while (!queue_.push(Address(p, i), Payload(p, i))) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
          }
          queue_.signal();
        }
      });
    }
  }
  void JoinPushers() {
    for (auto& thread : pushers_) {
      thread.join();
    }
    pushers_.clear();
  }

  std::vector<std::thread> pushers_;
};

TEST_F(TransportRecoveryTest, RateLimitedRouterReceivesEveryTelegramExactlyOnce) {
  router_.set_ack_delay_ms(3);  // a slow device: each ACK takes 3 ms

  TunnelingBusLink link(RouterOptions(router_.port()));
  ReaderThread reader(link, cache_, queue_);

  constexpr int kProducers = 4;
  constexpr int kPerProducer = 25;
  PushConcurrently(kProducers, kPerProducer);
  JoinPushers();

  const bool all_delivered =
      WaitUntil([this] { return router_.delivered().size() >= kProducers * kPerProducer; }, 30000);

  ASSERT_TRUE(all_delivered) << "delivered " << router_.delivered().size();
  const auto delivered = router_.delivered();
  ASSERT_EQ(delivered.size(), static_cast<std::size_t>(kProducers * kPerProducer));

  std::vector<int> last(kProducers, -1);
  for (const auto& [addr, apdu] : delivered) {
    ASSERT_EQ(apdu.size(), 5U);
    const int producer = apdu[2];
    const int sequence = (apdu[3] << 8) | apdu[4];
    EXPECT_EQ(addr, Address(producer, sequence));
    EXPECT_GT(sequence, last[producer]) << "order broken for producer " << producer;
    last[producer] = sequence;
  }
  // "Exactly once" is an eventual property, not an instantaneous one: the
  // router records the telegram *before* it ACKs, and the reader drops the slot
  // only after the ACK arrived.  Asserting the queue length immediately after
  // the last delivery therefore races against that handshake — it did fail
  // intermittently on fast Release builds in CI.
  EXPECT_TRUE(WaitUntil([this] { return queue_.size() == 0; }, 5000))
      << "telegrams still queued: " << queue_.size();
}

TEST_F(TransportRecoveryTest, SilentDeviceLosesNothingAndRecoversInOrder) {
  router_.set_silent(true);  // cable pulled / device rebooting

  TunnelingBusLink link(RouterOptions(router_.port()));
  ReaderThread reader(link, cache_, queue_);

  constexpr int kCount = 10;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(queue_.push(Address(1, i), Payload(1, i)));
  }
  queue_.signal();

  // While the device is silent nothing arrives, but nothing is dropped either.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_TRUE(router_.delivered().empty());
  EXPECT_EQ(queue_.size(), static_cast<std::size_t>(kCount));

  router_.set_silent(false);  // device is back

  const bool recovered = WaitUntil([this] { return router_.delivered().size() >= kCount; }, 30000);
  ASSERT_TRUE(recovered) << "delivered " << router_.delivered().size();
  const auto delivered = router_.delivered();
  ASSERT_EQ(delivered.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    const int sequence = (delivered[static_cast<std::size_t>(i)].second[3] << 8) |
                         delivered[static_cast<std::size_t>(i)].second[4];
    EXPECT_EQ(sequence, i) << "the backlog must keep its order";
  }
}

TEST_F(TransportRecoveryTest, DeviceDroppingTheTunnelIsReconnected) {
  router_.set_ack_delay_ms(1);
  TunnelingBusLink link(RouterOptions(router_.port()));
  ReaderThread reader(link, cache_, queue_);

  ASSERT_TRUE(queue_.push(Address(2, 0), Payload(2, 0)));
  queue_.signal();
  ASSERT_TRUE(WaitUntil([this] { return router_.delivered().size() >= 1; }, 10000));
  const int channels_before = router_.channels_used();

  // The device forgets the tunnel (reboot): the reader must reconnect and the
  // next write must go through on the new channel.
  router_.drop_tunnel();
  ASSERT_TRUE(WaitUntil(
      [this, channels_before] { return router_.channels_used() > channels_before; }, 10000))
      << "no reconnect after the device dropped the tunnel";

  ASSERT_TRUE(queue_.push(Address(2, 1), Payload(2, 1)));
  queue_.signal();
  const bool delivered_after = WaitUntil([this] { return router_.delivered().size() >= 2; }, 10000);

  EXPECT_TRUE(delivered_after) << "write after reconnect was not delivered";
  EXPECT_GE(router_.connect_requests(), 2);
}

}  // namespace
}  // namespace cvknx
