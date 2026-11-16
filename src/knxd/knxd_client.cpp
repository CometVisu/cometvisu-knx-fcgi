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
 * @file knxd_client.cpp
 * @brief Single-connection knxd client — group socket only.
 *
 * Implements the eibd client protocol over a Unix domain socket.
 * All I/O is non-blocking (O_NONBLOCK) for efficient poll()-based
 * waiting in the ReadHandler.  Rate-limiting (50 ms) on writes
 * prevents IP tunnel flooding.
 */

#include "knxd_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

#include "../util/debug_log.h"
#include "../util/hex.h"
#include "knxd_protocol.h"

namespace cvknx {

// ---- Internal state (PIMPL) ----

struct KnxdClient::Impl {
  Impl() = default;
  ~Impl() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  int fd = -1;                        // Unix socket to knxd
  bool group_socket_open = false;     // OPEN_GROUPCON succeeded
  int handshake_timeout_ms = 5000;    // OPEN_GROUPCON reply deadline
  std::string socket_path_;           // stored for reconnect
  bool write_only_ = false;           // stored for reconnect
  std::vector<uint8_t> read_buffer_;  // buffered partial reads (non-blocking)
  uint64_t telegram_count_ = 0;       // total APDU_PACKET received

  mutable std::recursive_mutex mutex;  // serializes socket access
};

// ---- Internal helpers ----

namespace {

/// Write all bytes to a socket, handling EAGAIN in non-blocking mode.
/// Retries with poll(POLLOUT) when the kernel send buffer is full.
/// Returns true if all bytes were written.
bool write_all(int fd, const uint8_t* data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    ssize_t written = ::write(fd, &data[offset], len - offset);
    if (written < 0) {
      // GCOV_EXCL_START - a write of at most one eibd message (<= 20 bytes) fits
      // into the socket buffer, so EAGAIN and EINTR cannot be provoked here.
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 5000) <= 0) {
          return false;
        }
        continue;
      }
      return false;
      // GCOV_EXCL_STOP
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

/// Shrink a vector if capacity far exceeds usage — prevents memory
/// retention after traffic bursts on long-running embedded systems.
inline void shrink_if_large(std::vector<uint8_t>& buf) {
  // The 4x rule is sufficient: the buffer only ever grows by whole eibd
  // messages, and a message is at most 64 KiB, so capacity can never exceed
  // size by more than that.
  // NOLINTNEXTLINE(bugprone-implicit-widening-of-multiplication-result)
  if (buf.capacity() > static_cast<size_t>(buf.size()) * 4) {
    buf.shrink_to_fit();
  }
}

/// Result of a single non-blocking read attempt.
enum class ReadOutcome : std::uint8_t { Data, Eof, WouldBlock, Error };

/// Pull whatever bytes are currently available into @p buffer.
ReadOutcome read_some(int fd, std::vector<uint8_t>& buffer) {
  std::array<uint8_t, 4096> tmp{};
  // NOLINTNEXTLINE(bugprone-signed-char-misuse)
  const ssize_t n = ::read(fd, tmp.data(), tmp.size());
  if (n > 0) {
    size_t new_size = buffer.size() + static_cast<size_t>(n);
    // GCOV_EXCL_START - an eibd message carries a 16-bit length, so at most
    // ~64 KiB can accumulate and the 1 MiB ceiling is never reached.
    if (new_size > kMaxReadBufferSize) {
      size_t excess = new_size - kMaxReadBufferSize;
      if (excess >= buffer.size()) {
        buffer.clear();
      } else {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(excess));
      }
    }
    // GCOV_EXCL_STOP
    // A read may fill the chunk completely, so n can equal tmp.size(): the end
    // of the range must be formed as tmp.data() + n, because indexing tmp[n]
    // would be out of bounds.
    buffer.insert(buffer.end(), tmp.data(), tmp.data() + static_cast<size_t>(n));
    return ReadOutcome::Data;
  }
  if (n == 0) {
    return ReadOutcome::Eof;
  }
  // On Linux EAGAIN and EWOULDBLOCK are the same value, so only the first half
  // of the condition can ever be evaluated.
  if (errno == EAGAIN || errno == EWOULDBLOCK) {  // GCOV_EXCL_BR_LINE
    return ReadOutcome::WouldBlock;
  }
  if (errno == EINTR) {              // GCOV_EXCL_BR_LINE - no signal during the read
    return ReadOutcome::WouldBlock;  // GCOV_EXCL_LINE
  }
  return ReadOutcome::Error;
}

/// Read one complete eibd message (2-byte big-endian length prefix)
/// from a non-blocking socket.  Uses an accumulation buffer for partial
/// reads.  Returns std::nullopt if no complete message is available yet.
std::optional<std::vector<uint8_t>> read_message(int fd, std::vector<uint8_t>& buffer) {
  while (true) {
    auto msg = try_extract_message(buffer);
    if (msg.has_value()) {
      shrink_if_large(buffer);
      return msg;
    }
    if (read_some(fd, buffer) != ReadOutcome::Data) {
      return std::nullopt;
    }
  }
}

/// Read one complete eibd message, waiting at most @p timeout_ms for it.
///
/// Used for the group socket handshake, where the fd may still be in blocking
/// mode: without a deadline a knxd that accepts the connection but never
/// answers keeps the caller (and therefore the whole /w request) blocked
/// forever.
std::optional<std::vector<uint8_t>> read_message_deadline(int fd, std::vector<uint8_t>& buffer,
                                                          int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));

  while (true) {
    auto msg = try_extract_message(buffer);
    if (msg.has_value()) {
      shrink_if_large(buffer);
      return msg;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {  // GCOV_EXCL_BR_LINE - poll() hits the deadline first
      return std::nullopt;  // GCOV_EXCL_LINE
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int poll_ms = static_cast<int>(std::max<int64_t>(1, remaining_ms));

    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, poll_ms);
    if (ready == 0) {
      return std::nullopt;  // deadline reached
    }
    // GCOV_EXCL_START - poll() on a valid socket only fails for EINTR
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::nullopt;
    }
    // GCOV_EXCL_STOP

    switch (read_some(fd, buffer)) {  // GCOV_EXCL_BR_LINE - all outcomes handled
      case ReadOutcome::Data:
      case ReadOutcome::WouldBlock:
        break;
      case ReadOutcome::Eof:
      case ReadOutcome::Error:
        return std::nullopt;
    }
  }
}

}  // namespace

// ---- Public API ----

KnxdClient::KnxdClient(std::string socket_path) : impl_(std::make_unique<Impl>()) {
  impl_->socket_path_ = std::move(socket_path);
}
KnxdClient::~KnxdClient() = default;
KnxdClient::KnxdClient(KnxdClient&& other) noexcept : impl_(std::move(other.impl_)) {}
KnxdClient& KnxdClient::operator=(KnxdClient&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool KnxdClient::connect(std::string_view socket_path) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);

  if (impl_->fd >= 0) {
    disconnect();
  }

  impl_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->fd < 0) {  // GCOV_EXCL_BR_LINE - only fails on fd exhaustion
    return false;       // GCOV_EXCL_LINE
  }

  struct sockaddr_un addr {};
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    // An over-long path cannot be represented: release the socket again,
    // otherwise every rejected attempt would leak one descriptor.
    ::close(impl_->fd);
    impl_->fd = -1;
    return false;
  }
  std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());

  // Remember the path even when this attempt fails: a later connect() (the
  // cache reader is the single reconnect authority) needs it to recover once
  // knxd is back (e.g. "Link down, terminating" at startup).
  impl_->socket_path_ = socket_path;

  // Non-blocking connect with 5-second timeout.
  int flags = ::fcntl(impl_->fd, F_GETFL, 0);
  if (flags >= 0) {  // GCOV_EXCL_BR_LINE - fcntl() on a fresh fd cannot fail
    ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  int ret = ::connect(impl_->fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  // errno can never be EINPROGRESS for AF_UNIX (see the block below).
  if (ret < 0 && errno != EINPROGRESS) {  // GCOV_EXCL_BR_LINE
    ::close(impl_->fd);
    impl_->fd = -1;
    return false;
  }

  // GCOV_EXCL_START - an AF_UNIX connect() completes synchronously; EINPROGRESS
  // is a TCP-only outcome and no test can produce it here.
  if (ret < 0) {
    struct pollfd pfd {};
    pfd.fd = impl_->fd;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, 5000) <= 0) {
      ::close(impl_->fd);
      impl_->fd = -1;
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(impl_->fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
      ::close(impl_->fd);
      impl_->fd = -1;
      return false;
    }
  }
  // GCOV_EXCL_STOP

  // Restore blocking mode (caller can override via set_nonblocking).
  if (flags >= 0) {  // GCOV_EXCL_BR_LINE - same as above
    ::fcntl(impl_->fd, F_SETFL, flags & ~O_NONBLOCK);
  }

  return true;
}

void KnxdClient::disconnect() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->group_socket_open = false;
  impl_->read_buffer_.clear();
}

bool KnxdClient::is_connected() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->fd >= 0;
}

bool KnxdClient::connect() {
  if (impl_->socket_path_.empty()) {
    return false;
  }
  // A usable knxd link is the socket plus an accepted group socket handshake;
  // without it no telegram would ever be delivered.
  if (!connect(std::string_view{impl_->socket_path_})) {
    return false;
  }
  if (!open_group_socket(false)) {
    disconnect();
    return false;
  }
  return true;
}

std::string KnxdClient::label() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return "knxd at " + impl_->socket_path_;
}

bool KnxdClient::open_group_socket(bool write_only) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  // is_connected() already guarantees fd >= 0, so the helper's own guard is
  // never reached from here.
  if (!is_connected()) {  // GCOV_EXCL_BR_LINE
    return false;         // GCOV_EXCL_LINE
  }

  impl_->write_only_ = write_only;
  auto msg = build_open_groupcon(write_only);
  DebugLog::knxd_send("open_group_socket", "-",
                      write_only ? "write_only=true" : "write_only=false");

  if (!write_all(impl_->fd, msg.data(), msg.size())) {
    return false;
  }

  auto resp = read_message_deadline(impl_->fd, impl_->read_buffer_, impl_->handshake_timeout_ms);
  if (!resp) {
    // knxd accepted the connection but did not answer in time — treat the
    // handshake as failed instead of blocking the caller forever.
    return false;
  }

  uint16_t resp_type = 0;
  std::vector<uint8_t> resp_data;
  if (!parse_eibd_message(*resp, resp_type, resp_data)) {
    return false;
  }

  impl_->group_socket_open = (resp_type == EibMessageType::OPEN_GROUPCON);
  return impl_->group_socket_open;
}

bool KnxdClient::send_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);

  // No rate limiting and no reconnect here: both belong to the cache reader
  // loop.  Sleeping in this function would stall the *receive* path (the same
  // thread that pumps inbound telegrams), and a reconnect with its handshake
  // deadline would block it for seconds.  The reader paces sends via
  // min_send_interval_ms() and reconnects when is_connected() turns false.
  if (!is_connected() || !impl_->group_socket_open) {
    return false;
  }

  auto addr_str = KnxGroupAddress::from_eibaddr(group_addr).to_string();
  DebugLog::knxd_send("group_packet", addr_str, "apdu=" + hex_encode(apdu.data(), apdu.size()));

  const auto msg = build_group_packet(group_addr, apdu);
  if (!write_all(impl_->fd, msg.data(), msg.size())) {
    // Write failed (e.g. EPIPE after knxd restarted on a still-valid fd).
    // Drop the socket so the reader notices and reconnects; the telegram is
    // still in the shared ring and is sent again afterwards.
    disconnect();
    return false;
  }
  return true;
}

bool KnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);

  if (!is_connected() || !impl_->group_socket_open) {
    return false;
  }

  auto msg = read_message(impl_->fd, impl_->read_buffer_);
  if (!msg) {
    // A dead socket must become visible through is_connected(), so the reader
    // reconnects.  Reconnecting here (with its handshake deadline) would block
    // the receive path.
    struct pollfd pfd {};
    pfd.fd = impl_->fd;
    // NOLINTNEXTLINE(readability-implicit-bool-conversion)
    // A dead socket reports POLLHUP; POLLERR without POLLHUP cannot occur for a
    // stream socket whose peer went away, so only the HUP half is reachable.
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0) {  // GCOV_EXCL_BR_LINE
      disconnect();
    }
    return false;
  }

  uint16_t msg_type = 0;
  std::vector<uint8_t> msg_data;
  if (!parse_eibd_message(*msg, msg_type, msg_data)) {
    return false;
  }

  // APDU_PACKET: knxd delivers bus telegrams in this format.
  // Wire format: src_pa(2) + dst_ga(2) + apdu(N)
  if (msg_type == EibMessageType::APDU_PACKET && msg_data.size() >= 6) {
    out_group_addr = static_cast<uint16_t>((msg_data[2] << 8) | msg_data[3]);
    out_apdu.assign(msg_data.begin() + 4, msg_data.end());
    DebugLog::knxd_recv("apdu_packet", KnxGroupAddress::from_eibaddr(out_group_addr).to_string(),
                        hex_encode(out_apdu.data(), out_apdu.size()));
    impl_->telegram_count_++;
    return true;
  }

  // GROUP_PACKET (injected telegrams, e.g. knxtool groupswrite local).
  if (msg_type == EibMessageType::GROUP_PACKET && msg_data.size() >= 6) {
    out_group_addr = static_cast<uint16_t>((msg_data[2] << 8) | msg_data[3]);
    out_apdu.assign(msg_data.begin() + 4, msg_data.end());
    DebugLog::knxd_recv("group_packet_injected",
                        KnxGroupAddress::from_eibaddr(out_group_addr).to_string(),
                        hex_encode(out_apdu.data(), out_apdu.size()));
    impl_->telegram_count_++;
    return true;
  }

  return false;
}

int KnxdClient::get_fd() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->fd;
}

uint64_t KnxdClient::get_telegram_count() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->telegram_count_;
}

void KnxdClient::set_handshake_timeout_ms(int timeout_ms) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  impl_->handshake_timeout_ms = std::max(1, timeout_ms);
}

void KnxdClient::set_nonblocking(bool enable) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->fd < 0) {
    return;
  }
  int flags = ::fcntl(impl_->fd, F_GETFL, 0);
  if (flags < 0) {
    return;
  }
  if (enable) {
    ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
  } else {
    ::fcntl(impl_->fd, F_SETFL, flags & ~O_NONBLOCK);
  }
}

KnxdClient::WaitResult KnxdClient::wait_for_activity(int timeout_ms) {
  // Poll the group socket fd only.  The kernel puts the process to sleep
  // until data arrives or the timeout expires — zero CPU.
  int fd = -1;
  {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    fd = impl_->fd;
  }
  if (fd < 0) {
    // Not connected.  This MUST be reported as an error: reporting it as a
    // plain timeout makes the cache reader retry poll() in a tight loop and
    // burn 100% CPU while knxd is missing.
    return WaitResult::Error;
  }

  struct pollfd pfd {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int ret = ::poll(&pfd, 1, timeout_ms);
  if (ret < 0) {  // GCOV_EXCL_BR_LINE - poll() on a valid fd only fails for EINTR
    return (errno == EINTR) ? WaitResult::Timeout : WaitResult::Error;  // GCOV_EXCL_LINE
  }
  if (ret == 0) {
    return WaitResult::Timeout;
  }

  // Check for a dead connection BEFORE looking at POLLIN: a socket whose peer
  // went away reports POLLIN|POLLHUP, and treating that as "data available"
  // loops forever on the EOF.
  if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
    return WaitResult::Error;
  }
  // poll() reports POLLIN for every readable socket, so the timeout arm is a
  // safety net rather than a reachable outcome.
  return ((pfd.revents & POLLIN) != 0) ? WaitResult::GroupData
                                       : WaitResult::Timeout;  // GCOV_EXCL_LINE
}

}  // namespace cvknx