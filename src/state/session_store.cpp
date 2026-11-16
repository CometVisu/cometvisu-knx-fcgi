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
 * @file session_store.cpp
 * @brief Shared-memory session store implementation.
 */

#include "session_store.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <random>

namespace cvknx {
namespace {

SharedSessionEntry* entry_array(SharedSessionData* data) {
  return reinterpret_cast<SharedSessionEntry*>(data + 1);
}

std::size_t clamp_capacity(std::size_t capacity) {
  return std::clamp(capacity, kMinMaxSessions, kMaxMaxSessions);
}

std::uint32_t now_seconds() {
  return static_cast<std::uint32_t>(std::time(nullptr));
}

/// Seed the per-process id generator.
///
/// Several entropy sources are mixed in: std::random_device may be
/// deterministic on embedded systems, so the PID and two clock readings are
/// added as well.
std::mt19937_64 make_id_generator() {
  std::random_device rd;
  std::seed_seq seed{
      static_cast<unsigned>(rd()), static_cast<unsigned>(::getpid()),
      static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()),
      static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() >> 32)};
  return std::mt19937_64(seed);
}

/// A fresh 16-character hex session id (per-process generator; a fork copies
/// the state, the PID does not).
std::string generate_session_id() {
  // The guard of a thread_local is taken once per thread, so the "already
  // initialized" arm cannot appear inside a single test binary.
  static thread_local std::mt19937_64 gen = make_id_generator();  // GCOV_EXCL_BR_LINE
  static thread_local std::uniform_int_distribution<std::uint64_t> dist;

  const std::uint64_t num = dist(gen);
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string text;
  text.reserve(kSessionIdChars);
  for (std::size_t i = 0; i < kSessionIdChars; ++i) {
    text.push_back(kHex[(num >> (60 - i * 4)) & 0xF]);
  }
  return text;
}

/// Lock helper for the process-shared, robust session mutex.
///
/// `acquired()` is false only for an unrecoverable error, and the destructor
/// then leaves the mutex alone — unlocking a mutex you do not own is undefined
/// behaviour.
class SessionLock {
public:
  explicit SessionLock(pthread_mutex_t& mutex) : mutex_(mutex) {
    const int rc = ::pthread_mutex_lock(&mutex_);
    owned_ = (rc == 0);
    if (rc == EOWNERDEAD) {
      // The previous owner died mid-operation; the slot array is POD and
      // updated under the lock, so the state is still consistent.
      ::pthread_mutex_consistent(&mutex_);
      owned_ = true;
    }
  }
  ~SessionLock() {
    if (owned_) {  // GCOV_EXCL_BR_LINE - a failed lock needs an invalid mutex
      ::pthread_mutex_unlock(&mutex_);
    }
  }
  SessionLock(const SessionLock&) = delete;
  SessionLock& operator=(const SessionLock&) = delete;
  SessionLock(SessionLock&&) = delete;
  SessionLock& operator=(SessionLock&&) = delete;

  [[nodiscard]] bool acquired() const { return owned_; }

private:
  pthread_mutex_t& mutex_;
  bool owned_ = false;
};

}  // namespace

std::size_t session_region_size(std::size_t max_sessions) {
  return sizeof(SharedSessionData) + clamp_capacity(max_sessions) * sizeof(SharedSessionEntry);
}

SessionStore::SessionStore(int session_ttl_sec, std::size_t max_sessions) {
  const std::size_t slots = clamp_capacity(max_sessions);
  const std::size_t bytes = session_region_size(slots);

  void* region = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // GCOV_EXCL_START - mmap() only fails on address-space exhaustion
  if (region == MAP_FAILED) {
    return;
  }
  // GCOV_EXCL_STOP

  auto* d = new (region) SharedSessionData{};
  d->capacity = static_cast<std::uint32_t>(slots);
  d->ttl_sec = static_cast<std::uint32_t>(std::max(0, session_ttl_sec));

  ::pthread_mutexattr_t attr;
  ::pthread_mutexattr_init(&attr);
  ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  // GCOV_EXCL_START - pthread_mutex_init() only fails on resource exhaustion
  if (::pthread_mutex_init(&d->mutex, &attr) != 0) {
    ::pthread_mutexattr_destroy(&attr);
    ::munmap(region, bytes);
    return;
  }
  // GCOV_EXCL_STOP
  ::pthread_mutexattr_destroy(&attr);

  data_ = d;
  owns_mmap_ = true;
  bytes_ = bytes;
}

SessionStore::~SessionStore() {
  // Only the owner unmaps, and the process-shared mutex is deliberately left
  // alone: a worker that unwinds normally must not tear down the object the
  // whole pool uses.  (All children exit through _Exit() anyway.)
  if (data_ != nullptr && owns_mmap_) {
    ::munmap(data_, bytes_);
    data_ = nullptr;
    owns_mmap_ = false;
    bytes_ = 0;
  }
}

void SessionStore::attach(SharedSessionData* data) {
  data_ = data;
  owns_mmap_ = false;
  bytes_ = 0;
}

// ---- Internal helpers (caller holds the mutex) ----

int32_t SessionStore::find_locked(std::string_view id) const {
  const SharedSessionEntry* entries = entry_array(data_);
  for (std::uint32_t i = 0; i < data_->capacity; ++i) {
    if (entries[i].id[0] == '\0') {
      continue;
    }
    if (id == std::string_view{entries[i].id, std::strlen(entries[i].id)}) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

std::size_t SessionStore::cleanup_locked(std::uint32_t now) {
  SharedSessionEntry* entries = entry_array(data_);
  std::size_t removed = 0;
  for (std::uint32_t i = 0; i < data_->capacity; ++i) {
    if (entries[i].id[0] == '\0') {
      continue;
    }
    // Signed difference: a backwards clock step must not make a session look
    // expired (the unsigned value would wrap to a huge age).
    const std::int64_t age =
        static_cast<std::int64_t>(now) - static_cast<std::int64_t>(entries[i].created);
    if (age >= static_cast<std::int64_t>(data_->ttl_sec)) {
      entries[i] = SharedSessionEntry{};
      --data_->count;
      ++removed;
    }
  }
  return removed;
}

int32_t SessionStore::allocate_locked() {
  SharedSessionEntry* entries = entry_array(data_);
  // Reuse a free slot first.
  for (std::uint32_t i = 0; i < data_->capacity; ++i) {
    if (entries[i].id[0] == '\0') {
      ++data_->count;
      return static_cast<int32_t>(i);
    }
  }
  // Full: evict the oldest session.  The creation sequence, not the wall
  // clock, decides — several sessions created within the same second must
  // still be evicted in the order they were created.  Every slot is occupied
  // here (the loop above returned the first free one).
  int32_t oldest = 0;
  for (std::uint32_t i = 1; i < data_->capacity; ++i) {
    if (entries[i].seq < entries[oldest].seq) {
      oldest = static_cast<int32_t>(i);
    }
  }
  entries[oldest] = SharedSessionEntry{};
  ++data_->evictions;
  return oldest;
}

// ---- Public API ----

std::string SessionStore::create_session(bool anonymous) {
  if (anonymous) {
    return std::string{kAnonymousSessionId};
  }
  if (data_ == nullptr) {
    return {};  // GCOV_EXCL_LINE - the region is created in the constructor
  }

  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return {};
  }
  // GCOV_EXCL_STOP

  const std::uint32_t now = now_seconds();
  (void)cleanup_locked(now);

  // Collisions are astronomically unlikely, but a duplicate id would silently
  // merge two clients into one session, so it is checked.
  std::string id = generate_session_id();
  while (find_locked(id) >= 0) {  // GCOV_EXCL_BR_LINE - a 64-bit collision cannot be produced
    id = generate_session_id();   // GCOV_EXCL_LINE
  }

  const int32_t slot = allocate_locked();
  SharedSessionEntry& entry = entry_array(data_)[slot];
  entry = SharedSessionEntry{};
  std::memcpy(entry.id, id.c_str(), std::min(id.size(), kSessionIdChars));
  entry.created = now;
  entry.last_used = now;
  entry.seq = data_->next_seq++;
  return id;
}

bool SessionStore::is_valid(std::string_view session_id) {
  if (session_id == kAnonymousSessionId) {
    return true;
  }
  if (data_ == nullptr) {
    return false;
  }

  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return false;
  }
  // GCOV_EXCL_STOP

  const int32_t slot = find_locked(session_id);
  if (slot < 0) {
    return false;
  }

  const std::uint32_t now = now_seconds();
  SharedSessionEntry& entry = entry_array(data_)[slot];
  // Signed difference — see cleanup_locked(): a clock step backwards must not
  // invalidate every live session.
  const std::int64_t age =
      static_cast<std::int64_t>(now) - static_cast<std::int64_t>(entry.created);
  if (age >= static_cast<std::int64_t>(data_->ttl_sec)) {
    entry = SharedSessionEntry{};
    --data_->count;
    return false;
  }
  entry.last_used = now;
  return true;
}

void SessionStore::remove(std::string_view session_id) {
  if (session_id == kAnonymousSessionId || data_ == nullptr) {
    return;
  }
  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return;
  }
  // GCOV_EXCL_STOP
  const int32_t slot = find_locked(session_id);
  if (slot >= 0) {
    entry_array(data_)[slot] = SharedSessionEntry{};
    --data_->count;
  }
}

void SessionStore::cleanup_expired() {
  if (data_ == nullptr) {
    return;
  }
  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return;
  }
  // GCOV_EXCL_STOP
  (void)cleanup_locked(now_seconds());
}

std::size_t SessionStore::count() const {
  if (data_ == nullptr) {
    return 0;
  }
  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return 0;
  }
  // GCOV_EXCL_STOP
  return data_->count;
}

std::size_t SessionStore::capacity() const {
  return data_ == nullptr ? 0 : data_->capacity;
}

std::uint64_t SessionStore::evictions() const {
  if (data_ == nullptr) {
    return 0;
  }
  SessionLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return 0;
  }
  // GCOV_EXCL_STOP
  return data_->evictions;
}

}  // namespace cvknx
