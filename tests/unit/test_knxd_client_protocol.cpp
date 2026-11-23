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
 * @file test_knxd_client_protocol.cpp
 * @brief Protocol-level tests for KnxdClient against a scripted fake knxd.
 *
 * The fake server speaks the eibd wire format (2-byte length prefix + payload)
 * so every client code path — handshake, send, receive, reconnect after a
 * broken socket and the malformed-input cases — can be driven from a test.
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/knxd/knxd_client.h"
#include "../../src/knxd/knxd_protocol.h"

namespace cvknx {
namespace {

using Bytes = std::vector<uint8_t>;

/// Joins a thread on scope exit — a failed ASSERT must not terminate the run.
class Joiner {
public:
  explicit Joiner(std::thread thread) : thread_(std::move(thread)) {}
  ~Joiner() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  Joiner(const Joiner&) = delete;
  Joiner& operator=(const Joiner&) = delete;
  Joiner(Joiner&&) = delete;
  Joiner& operator=(Joiner&&) = delete;

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  std::thread thread_;
};

/// Fake knxd: a Unix stream socket that answers with scripted bytes.
class FakeKnxdServer {
public:
  FakeKnxdServer() : fd_(::socket(AF_UNIX, SOCK_STREAM, 0)) {
    static int counter = 0;
    path_ = "/tmp/cvknx-proto-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) +
            ".sock";
    if (fd_ < 0) {
      return;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (::listen(fd_, 4) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  ~FakeKnxdServer() {
    if (peer_ >= 0) {
      ::close(peer_);
    }
    shutdown_listener();
  }

  FakeKnxdServer(const FakeKnxdServer&) = delete;
  FakeKnxdServer& operator=(const FakeKnxdServer&) = delete;
  FakeKnxdServer(FakeKnxdServer&&) = delete;
  FakeKnxdServer& operator=(FakeKnxdServer&&) = delete;

  [[nodiscard]] bool is_valid() const { return fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] int peer() const { return peer_; }

  int accept_one() {
    peer_ = ::accept(fd_, nullptr, nullptr);
    return peer_;
  }

  void close_peer() {
    if (peer_ >= 0) {
      ::close(peer_);
      peer_ = -1;
    }
  }

  /// Stop listening — further reconnects cannot succeed.
  void shutdown_listener() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
      ::unlink(path_.c_str());
    }
  }

  /// Read one eibd message (2-byte big-endian length + payload).
  bool receive_message(Bytes& out) {
    if (peer_ < 0) {
      return false;
    }
    Bytes buffer;
    for (;;) {
      auto message = try_extract_message(buffer);
      if (message.has_value()) {
        out = *message;
        return true;
      }
      uint8_t chunk[256] = {};
      const ssize_t n = ::read(peer_, chunk, sizeof(chunk));
      if (n <= 0) {
        return false;
      }
      buffer.insert(buffer.end(), chunk, chunk + static_cast<std::ptrdiff_t>(n));
    }
  }

  bool send_bytes(const Bytes& bytes) const {
    if (peer_ < 0) {
      return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const ssize_t written = ::write(peer_, bytes.data() + offset, bytes.size() - offset);
      if (written <= 0) {
        return false;
      }
      offset += static_cast<size_t>(written);
    }
    return true;
  }

  bool send_message(uint16_t type, const Bytes& payload) const {
    return send_bytes(build_eibd_message(type, payload));
  }

private:
  std::string path_;
  int fd_ = -1;
  int peer_ = -1;
};

/// Payload of an APDU_PACKET: src_pa(2) dst_ga(2) apdu(N).
Bytes apdu_payload(uint16_t group, const Bytes& apdu) {
  Bytes payload{0x11, 0x01, static_cast<uint8_t>(group >> 8), static_cast<uint8_t>(group & 0xFF)};
  payload.insert(payload.end(), apdu.begin(), apdu.end());
  return payload;
}

/// Accept the connection and answer the group socket handshake.
bool accept_and_handshake(FakeKnxdServer& server, uint8_t expected_write_only) {
  if (server.accept_one() < 0) {
    return false;
  }
  Bytes request;
  if (!server.receive_message(request)) {
    return false;
  }
  uint16_t type = 0;
  Bytes data;
  if (!parse_eibd_message(request, type, data) || type != EibMessageType::OPEN_GROUPCON) {
    return false;
  }
  // knxd expects a 3-byte payload: [reserved:2][write_only:1].
  if (data.size() != 3 || data[2] != expected_write_only) {
    return false;
  }
  return server.send_message(EibMessageType::OPEN_GROUPCON, {});
}

class KnxdClientProtocolTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Writing to a socket whose peer vanished raises SIGPIPE, which would kill
    // the test binary.  Production ignores it the same way.
    previous_ = ::signal(SIGPIPE, SIG_IGN);
  }
  void TearDown() override { ::signal(SIGPIPE, previous_); }

private:
  void (*previous_)(int) = SIG_DFL;
};

// ================================================================
// Happy path: handshake, send, receive
// ================================================================

TEST_F(KnxdClientProtocolTest, ConnectsSendsAndReceives) {
  FakeKnxdServer server;
  ASSERT_TRUE(server.is_valid());
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  // ---- send: the server must receive a GROUP_PACKET ----
  ASSERT_TRUE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  Bytes request;
  ASSERT_TRUE(server.receive_message(request));
  uint16_t type = 0;
  Bytes data;
  ASSERT_TRUE(parse_eibd_message(request, type, data));
  EXPECT_EQ(type, EibMessageType::GROUP_PACKET);
  ASSERT_GE(data.size(), 4U);
  EXPECT_EQ((data[0] << 8) | data[1], 0x0A03);

  // ---- receive: an APDU_PACKET becomes a group telegram ----
  ASSERT_TRUE(server.send_message(EibMessageType::APDU_PACKET,
                                  apdu_payload(0x0B04, {0x00, 0x80, 0x0C, 0x6F})));
  uint16_t group = 0;
  Bytes apdu;
  ASSERT_TRUE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(group, 0x0B04);
  EXPECT_EQ(apdu, (Bytes{0x00, 0x80, 0x0C, 0x6F}));
  EXPECT_EQ(client.get_telegram_count(), 1U);
  EXPECT_EQ(client.label(), "knxd at " + server.path());

  // Nothing else pending.
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, AcceptsInjectedGroupPackets) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  ASSERT_TRUE(
      server.send_message(EibMessageType::GROUP_PACKET, apdu_payload(0x0A03, {0x00, 0x80, 0x01})));
  uint16_t group = 0;
  Bytes apdu;
  ASSERT_TRUE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(group, 0x0A03);
  EXPECT_EQ(client.get_telegram_count(), 1U);
}

TEST_F(KnxdClientProtocolTest, IgnoresUnknownMessageTypesAndShortPayloads) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  uint16_t group = 0;
  Bytes apdu;
  ASSERT_TRUE(server.send_message(0x0033, {0x00}));  // unknown message type
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  ASSERT_TRUE(server.send_message(EibMessageType::APDU_PACKET, {0x11, 0x01}));  // too short
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(client.get_telegram_count(), 0U);
}

TEST_F(KnxdClientProtocolTest, WriteOnlyHandshakeIsSentAsRequested) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x01); }));
  ASSERT_TRUE(client.connect(server.path()));
  EXPECT_TRUE(client.open_group_socket(true));
  server_thread.join();
  EXPECT_TRUE(client.is_connected());
}

// ================================================================
// Handshake failures
// ================================================================

TEST_F(KnxdClientProtocolTest, HandshakeFailsWhenTheReplyCannotBeParsed) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] {
    if (server.accept_one() < 0) {
      return;
    }
    Bytes request;
    (void)server.receive_message(request);
    // A one-byte payload cannot be a valid eibd message.
    (void)server.send_bytes({0x00, 0x01, 0x26});
  }));
  EXPECT_FALSE(client.connect());
  server_thread.join();
  EXPECT_FALSE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, HandshakeFailsWithoutAnyReply) {
  FakeKnxdServer server;
  KnxdClient client(server.path());
  client.set_handshake_timeout_ms(50);

  Joiner server_thread(std::thread([&server] { (void)server.accept_one(); }));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.connect());
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  server_thread.join();
  // The deadline must be honoured — a silent knxd may not block forever.
  EXPECT_LT(elapsed, 2000);
  EXPECT_FALSE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, ConnectRejectsAnOverlongSocketPath) {
  KnxdClient client;
  const std::string long_path(200, 'x');
  EXPECT_FALSE(client.connect(long_path));
  EXPECT_FALSE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, ConnectWithoutAnyPathFails) {
  // The default path is /run/knx, and whether anything answers there is a
  // property of the machine, not of the client.  The guard under test is "no
  // configured path → no connection attempt", so the path is set explicitly.
  KnxdClient client{""};
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.connect());
}

// ================================================================
// Recovery paths
// ================================================================

TEST_F(KnxdClientProtocolTest, SendDropsTheConnectionWhenTheWriteFails) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  // knxd restarted: the inherited fd is dead.  The client must NOT reconnect on
  // its own (that would block the receive path); it reports the failure and
  // drops the socket so the cache reader can reconnect.
  server.close_peer();
  const int broken_fd = client.get_fd();
  ASSERT_GE(broken_fd, 0);
  ASSERT_EQ(::close(broken_fd), 0);

  EXPECT_FALSE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  EXPECT_FALSE(client.is_connected()) << "a failed write must expose the dead link";
}

TEST_F(KnxdClientProtocolTest, SendFailsWithoutAConnection) {
  KnxdClient client("/tmp/cvknx-missing-send-" + std::to_string(::getpid()) + ".sock");
  EXPECT_FALSE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
}

TEST_F(KnxdClientProtocolTest, PollDropsTheConnectionWhenThePeerWentAway) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  // The peer closes the connection: the poll reports "nothing" and marks the
  // link dead, so the reader reconnects.
  server.close_peer();
  server.shutdown_listener();

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  EXPECT_FALSE(client.is_connected());
}

// ================================================================
// Transport pacing (the interval the cache reader honours)
// ================================================================

TEST_F(KnxdClientProtocolTest, DeclaresTheKnxdSendInterval) {
  // The 50 ms interval that protects knxd's IP tunnel is a *transport
  // property*: the cache reader waits between sends, this class never sleeps.
  KnxdClient client("/tmp/unused.sock");
  EXPECT_EQ(client.min_send_interval_ms(), 50);
}

TEST_F(KnxdClientProtocolTest, BackToBackWritesAreNotDelayed) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  ASSERT_TRUE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x01}));
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(client.send_group_packet(0x0A04, {0x00, 0x80, 0x02}));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  EXPECT_LT(elapsed, 40) << "sleeping here would stall the receive path";
}

TEST_F(KnxdClientProtocolTest, SettersAndGettersTolerateADisconnectedClient) {
  KnxdClient client("/tmp/does-not-exist-cvknx.sock");
  EXPECT_FALSE(client.is_connected());
  EXPECT_LT(client.get_fd(), 0);
  EXPECT_EQ(client.get_telegram_count(), 0U);
  client.set_nonblocking(true);   // must not crash without a socket
  client.set_nonblocking(false);  // ... in either direction
  client.disconnect();            // idempotent
  EXPECT_EQ(client.wait_for_activity(1), BusLink::WaitResult::Error);
}

// ================================================================
// Degenerate states and defensive paths
// ================================================================

TEST_F(KnxdClientProtocolTest, MoveConstructAndMoveAssign) {
  KnxdClient first("/tmp/cvknx-move-a.sock");
  KnxdClient moved(std::move(first));
  EXPECT_EQ(moved.label(), "knxd at /tmp/cvknx-move-a.sock");

  KnxdClient second("/tmp/cvknx-move-b.sock");
  second = std::move(moved);
  EXPECT_EQ(second.label(), "knxd at /tmp/cvknx-move-a.sock");

  // Self-assignment must not destroy the client.
  KnxdClient& alias = second;
  second = std::move(alias);
  EXPECT_EQ(second.label(), "knxd at /tmp/cvknx-move-a.sock");
}

TEST_F(KnxdClientProtocolTest, ConnectDisconnectsAStaleConnectionFirst) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  // A second connect() on a live client must close the old socket first.
  Joiner second_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  EXPECT_TRUE(client.connect());
  second_thread.join();
  EXPECT_TRUE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, OpenGroupSocketRequiresAConnection) {
  KnxdClient client("/tmp/cvknx-not-connected.sock");
  EXPECT_FALSE(client.open_group_socket(false));
}

TEST_F(KnxdClientProtocolTest, OpenGroupSocketReportsAWriteFailure) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  // Break the fd behind the client's back: write_all() cannot succeed.
  const int broken_fd = client.get_fd();
  ASSERT_GE(broken_fd, 0);
  ASSERT_EQ(::close(broken_fd), 0);
  EXPECT_FALSE(client.open_group_socket(false));
}

TEST_F(KnxdClientProtocolTest, ConnectFailsWhenTheHandshakeFails) {
  FakeKnxdServer server;
  KnxdClient client(server.path());
  client.set_handshake_timeout_ms(50);

  // Accept but never answer the handshake.
  Joiner server_thread(std::thread([&server] { (void)server.accept_one(); }));
  EXPECT_FALSE(client.connect());
  server_thread.join();
  EXPECT_FALSE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, EmptySocketPathMakesConnectFail) {
  KnxdClient client("");
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.connect());
}

TEST_F(KnxdClientProtocolTest, SendAndPollFailWithoutAConnection) {
  KnxdClient client("");  // no socket path: reconnect is impossible
  EXPECT_FALSE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x42}));
  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, PollDiscardsUnparsableMessages) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  // A complete frame whose payload is a single byte: extractable, but the
  // eibd message needs at least a 2-byte type field.
  ASSERT_TRUE(server.send_bytes({0x00, 0x01, 0x26}));
  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, HandshakeFailsWhenKnxdClosesImmediately) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] {
    if (server.accept_one() < 0) {
      return;
    }
    server.close_peer();  // EOF instead of an answer
  }));
  EXPECT_FALSE(client.connect());
  server_thread.join();
}

/// EOF while waiting for the OPEN_GROUPCON reply: the handshake fails instead
/// of blocking until the deadline.  (Deterministic variant: the server reads
/// the request first, so the client's write cannot fail before it reads.)
TEST_F(KnxdClientProtocolTest, HandshakeFailsWhenThePeerClosesAfterTheRequest) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] {
    if (server.accept_one() < 0) {
      return;
    }
    Bytes request;
    (void)server.receive_message(request);  // the OPEN_GROUPCON
    server.close_peer();                    // EOF instead of an answer
  }));
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
  server_thread.join();
}

TEST_F(KnxdClientProtocolTest, SetNonblockingCanBeToggledWhileConnected) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  client.set_nonblocking(true);
  client.set_nonblocking(false);
  EXPECT_TRUE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, HandshakeDeadlineIsCheckedAfterDataArrives) {
  FakeKnxdServer server;
  KnxdClient client(server.path());
  client.set_handshake_timeout_ms(60);

  Joiner server_thread(std::thread([&server] {
    if (server.accept_one() < 0) {
      return;
    }
    Bytes request;
    (void)server.receive_message(request);
    // Trickle an incomplete answer: the length prefix announces 8 bytes but
    // only one arrives, so the deadline has to end the wait.
    for (int i = 0; i < 20; ++i) {
      (void)server.send_bytes({0x00});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }));
  EXPECT_FALSE(client.connect());
  server_thread.join();
}

TEST_F(KnxdClientProtocolTest, SetNonblockingIgnoresABrokenDescriptor) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  const int broken_fd = client.get_fd();
  ASSERT_GE(broken_fd, 0);
  ASSERT_EQ(::close(broken_fd), 0);
  // fcntl() fails with EBADF; the call must simply do nothing.
  client.set_nonblocking(true);
  EXPECT_TRUE(true);
}

TEST_F(KnxdClientProtocolTest, ConnectFailsForAMissingSocketFile) {
  KnxdClient client;
  EXPECT_FALSE(client.connect("/tmp/cvknx-missing-" + std::to_string(::getpid()) + ".sock"));
  EXPECT_FALSE(client.is_connected());
}

TEST_F(KnxdClientProtocolTest, SendFailsWhenTheGroupSocketIsNotOpen) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  // connect(path) opens the socket only — the group socket handshake is open_group_socket().
  ASSERT_TRUE(client.connect(server.path()));
  EXPECT_FALSE(client.send_group_packet(0x0A03, {0x00, 0x80, 0x42}))
      << "without the group socket handshake no telegram may be reported as sent";
  client.disconnect();
}

TEST_F(KnxdClientProtocolTest, PollFailsAfterAnExplicitDisconnect) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  client.disconnect();
  ASSERT_FALSE(client.is_connected());
  server.shutdown_listener();

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, PollWithoutDataReturnsFalse) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, IgnoresShortInjectedGroupPackets) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  // GROUP_PACKET without src/dst: cannot be interpreted as a group telegram.
  ASSERT_TRUE(server.send_message(EibMessageType::GROUP_PACKET, {0x11, 0x01, 0x00}));
  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(client.get_telegram_count(), 0U);
}

TEST_F(KnxdClientProtocolTest, WaitForActivityReportsGroupData) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  ASSERT_TRUE(
      server.send_message(EibMessageType::APDU_PACKET, apdu_payload(0x0A03, {0x00, 0x80, 0x01})));
  EXPECT_EQ(client.wait_for_activity(500), BusLink::WaitResult::GroupData);
  EXPECT_EQ(client.wait_for_activity(20), BusLink::WaitResult::GroupData);
}

TEST_F(KnxdClientProtocolTest, PartialMessageIsNotTreatedAsAnError) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();
  client.set_nonblocking(true);

  // Announce 16 bytes but deliver only the prefix: the socket is readable, yet
  // there is no complete message — the client must simply report "nothing".
  // Length prefix 0x0010 = 16 payload bytes, then only the first half of the
  // message type field.
  ASSERT_TRUE(server.send_bytes({0x00, 0x10, 0x00, 0x25}));
  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));

  // Completing the message delivers the telegram.
  Bytes rest{0x11, 0x01, 0x0A, 0x03, 0x00, 0x80, 0x42};
  rest.resize(14, 0x00);
  ASSERT_TRUE(server.send_bytes(rest));
  EXPECT_TRUE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(group, 0x0A03);
}

TEST_F(KnxdClientProtocolTest, PollGivesUpWhenTheReconnectFails) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  // knxd is gone for good: the dead connection must not lead to a retry loop.
  server.close_peer();
  server.shutdown_listener();

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
}

TEST_F(KnxdClientProtocolTest, PollHandlesAnEofAndABrokenDescriptor) {
  {
    // EOF: knxd closed the connection while the reader was idle.
    FakeKnxdServer server;
    KnxdClient client(server.path());
    Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
    ASSERT_TRUE(client.connect());
    server_thread.join();
    server.close_peer();
    server.shutdown_listener();

    uint16_t group = 0;
    Bytes apdu;
    EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  }
  {
    // Broken descriptor: read() fails with EBADF instead of returning data.
    FakeKnxdServer server;
    KnxdClient client(server.path());
    Joiner server_thread(std::thread([&server] { (void)accept_and_handshake(server, 0x00); }));
    ASSERT_TRUE(client.connect());
    server_thread.join();
    server.shutdown_listener();

    const int broken_fd = client.get_fd();
    ASSERT_GE(broken_fd, 0);
    ASSERT_EQ(::close(broken_fd), 0);

    uint16_t group = 0;
    Bytes apdu;
    EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  }
}

TEST_F(KnxdClientProtocolTest, LargeMessageShrinksTheReadBuffer) {
  FakeKnxdServer server;
  KnxdClient client(server.path());

  Joiner server_thread(std::thread([&server] {
    if (server.accept_one() < 0) {
      return;
    }
    Bytes request;
    (void)server.receive_message(request);
    (void)server.send_message(EibMessageType::OPEN_GROUPCON, {});
    // A payload far beyond the 4 KiB read chunk: after it is consumed the
    // accumulation buffer is much larger than the next message needs.
    // [src_pa 2][dst_ga 2][apdu ...]
    Bytes payload(4 * 1024, 0x00);
    payload[2] = 0x0A;
    payload[3] = 0x03;
    (void)server.send_message(EibMessageType::GROUP_PACKET, payload);
  }));
  ASSERT_TRUE(client.connect());
  server_thread.join();

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_TRUE(client.poll_group_telegram(group, apdu));
  EXPECT_EQ(group, 0x0A03);
  EXPECT_EQ(apdu.size(), 4U * 1024 - 4);  // the payload after src_pa + dst_ga
}

TEST_F(KnxdClientProtocolTest, PollFailsWhenTheGroupSocketIsNotOpen) {
  FakeKnxdServer server;
  KnxdClient client(server.path());
  // connect(path) opens the socket only — a telegram must not be reported
  // before the group socket handshake.
  ASSERT_TRUE(client.connect(server.path()));
  server.shutdown_listener();

  uint16_t group = 0;
  Bytes apdu;
  EXPECT_FALSE(client.poll_group_telegram(group, apdu));
  client.disconnect();
}

}  // namespace
}  // namespace cvknx
