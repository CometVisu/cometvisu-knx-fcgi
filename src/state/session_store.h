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
 * @file session_store.h
 * @brief CometVisu session management — creation, validation, expiry.
 *
 * **Sessions must be shared by every worker process.**  A CometVisu client
 * logs in once (`/l`) and then sends `s=<id>` with every `/r` and `/w` — and
 * those requests are handled by *any* worker of the fork-based pool.  A
 * per-process store would make the session valid in exactly one of
 * `FCGI_WORKERS` processes (verified: with four workers every follow-up
 * request answered 401).  The store therefore lives in
 * mmap(MAP_SHARED|MAP_ANONYMOUS) memory, created before fork(), exactly like
 * the group cache and the TX ring.
 *
 * Sessions are identified by 16-character hex strings.  Anonymous sessions
 * use the special id "0", which is always valid and never occupies a slot.
 *
 * Capacity is a runtime value (`SESSION_MAX`): the slot array is sized once,
 * in the parent, before fork().  When the table is full the **oldest session**
 * is evicted, so a client that logs in again always finds room.
 *
 * Session lifetime is deliberately *not* tied to the long-poll timeout: a
 * session lives for `ttl` seconds (default 30 minutes, `SESSION_TTL_SEC`)
 * while no single HTTP request may wait longer than a few minutes.
 *
 * Thread/process safety: every public method takes the process-shared robust
 * mutex; there is no unlocked public mutator.
 */

#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace cvknx {

/// Default maximum number of concurrent named sessions.
inline constexpr std::size_t kDefaultMaxSessions = 10000;
/// Smallest accepted capacity (tests use tiny values to exercise eviction).
inline constexpr std::size_t kMinMaxSessions = 1;
/// Largest accepted capacity (guards against a nonsense environment value).
inline constexpr std::size_t kMaxMaxSessions = 1'000'000;
/// Length of a session id in characters (64 bits, hex encoded).
inline constexpr std::size_t kSessionIdChars = 16;
/// Anonymous session id — always valid, never stored.
inline constexpr std::string_view kAnonymousSessionId = "0";

/// One session slot.  Must be trivially copyable (shared memory).
struct SharedSessionEntry {
  /// Session id (NUL terminated).  An empty id marks a free slot.
  char id[kSessionIdChars + 1] = {};
  std::uint32_t created = 0;    ///< Unix time (s) of creation
  std::uint32_t last_used = 0;  ///< Unix time (s) of the last validation
  std::uint64_t seq = 0;        ///< creation order (monotonic, tie breaker)
};
static_assert(std::is_trivially_copyable_v<SharedSessionEntry>,
              "SharedSessionEntry must be POD for shared memory");

/// Fixed part of the shared session region; the slots follow it.
struct SharedSessionData {
  alignas(64) pthread_mutex_t mutex;
  std::uint32_t capacity = 0;   ///< slots in this region
  std::uint32_t count = 0;      ///< slots in use
  std::uint32_t ttl_sec = 0;    ///< session lifetime
  std::uint64_t next_seq = 1;   ///< next creation sequence number
  std::uint64_t evictions = 0;  ///< lifetime counter (diagnostics)
};

/// Size of the shared region for @p max_sessions slots.
[[nodiscard]] std::size_t session_region_size(std::size_t max_sessions);

/**
 * @brief Stores active CometVisu sessions in shared memory.
 *
 * The constructor creates the shared region, so it must run in the parent
 * process before fork().  `attach()` exists for tests (and for any future
 * helper process) that reuse an existing region.
 */
class SessionStore {
public:
  /// Default session TTL in seconds (30 minutes).
  static constexpr int kDefaultSessionTtlSec = 1800;

  /// Create the shared region.
  /// @param session_ttl_sec Lifetime of a session.
  /// @param max_sessions Number of concurrent sessions (oldest is evicted).
  explicit SessionStore(int session_ttl_sec = kDefaultSessionTtlSec,
                        std::size_t max_sessions = kDefaultMaxSessions);
  ~SessionStore();

  SessionStore(const SessionStore&) = delete;
  SessionStore& operator=(const SessionStore&) = delete;
  SessionStore(SessionStore&&) = delete;
  SessionStore& operator=(SessionStore&&) = delete;

  /// Attach to an existing region (tests, child processes).
  void attach(SharedSessionData* data);

  /// Whether the region exists (creation succeeded or attach() was called).
  [[nodiscard]] bool is_initialized() const { return data_ != nullptr; }

  /// Create a new session and return its id.
  /// @param anonymous If true, returns the always-valid anonymous id "0".
  /// @return the session id, or an empty string only when the shared region
  ///         could not be created (out of memory).
  [[nodiscard]] std::string create_session(bool anonymous = false);

  /// Check that a session exists and has not expired.  Expired sessions are
  /// removed on the way.
  [[nodiscard]] bool is_valid(std::string_view session_id);

  /// Remove a session (no-op for the anonymous id and unknown ids).
  void remove(std::string_view session_id);

  /// Remove every expired session.
  void cleanup_expired();

  /// Number of active sessions (the anonymous session is not counted).
  [[nodiscard]] std::size_t count() const;

  /// Number of slots in the region.
  [[nodiscard]] std::size_t capacity() const;

  /// Number of sessions dropped because the table was full.
  [[nodiscard]] std::uint64_t evictions() const;

  /// Raw region pointer (for passing to child processes).
  [[nodiscard]] SharedSessionData* data() { return data_; }

  /// Size of the shared region in bytes.
  [[nodiscard]] static std::size_t region_size(std::size_t max_sessions = kDefaultMaxSessions) {
    return session_region_size(max_sessions);
  }

private:
  /// Slot holding @p id, or -1.  Caller holds the mutex.
  [[nodiscard]] int32_t find_locked(std::string_view id) const;
  /// Free all expired slots and return the number removed.  Caller holds the mutex.
  std::size_t cleanup_locked(std::uint32_t now);
  /// Slot to use for a new session; evicts the oldest one when full.
  /// Caller holds the mutex.
  [[nodiscard]] int32_t allocate_locked();

  SharedSessionData* data_ = nullptr;
  bool owns_mmap_ = false;
  std::size_t bytes_ = 0;
};

}  // namespace cvknx
