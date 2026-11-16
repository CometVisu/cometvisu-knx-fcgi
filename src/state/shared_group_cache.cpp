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
 * @file shared_group_cache.cpp
 * @brief Shared-memory group cache: push, delta queries, LRU eviction.
 */

#include "shared_group_cache.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

namespace cvknx {
namespace {

/// Bucket heads follow the SharedCacheData header.
int32_t* bucket_array(SharedCacheData* data) {
  return reinterpret_cast<int32_t*>(data + 1);
}

/// Entry slots follow the bucket heads.
SharedCacheEntry* entry_array(SharedCacheData* data) {
  return reinterpret_cast<SharedCacheEntry*>(bucket_array(data) + data->capacity);
}

/// Hash bucket of a group address (capacity is not a power of two).
std::size_t bucket_index(const SharedCacheData* data, uint16_t addr) {
  return static_cast<std::size_t>(addr) % data->capacity;
}

/// Clamp an environment/user supplied capacity into the supported range.
std::size_t clamp_capacity(std::size_t capacity) {
  return std::clamp(capacity, kMinCacheCapacity, kMaxCacheCapacity);
}

/// Lock helper for the process-shared, robust cache mutex.
///
/// Recovers from a previous owner that died while holding the lock
/// (EOWNERDEAD).  `acquired()` is false only for an unrecoverable error, and
/// the destructor then leaves the mutex alone.
class CacheLock {
public:
  explicit CacheLock(pthread_mutex_t& mutex) : mutex_(mutex) {
    const int rc = ::pthread_mutex_lock(&mutex_);
    owned_ = (rc == 0);
    if (rc == EOWNERDEAD) {
      // Previous owner died while holding the lock.  The critical section may
      // have been interrupted halfway through a multi-step update, so the
      // caller has to repair the table — the mutex alone cannot.
      ::pthread_mutex_consistent(&mutex_);
      owned_ = true;
      owner_died_ = true;
    }
  }
  ~CacheLock() {
    if (owned_) {  // GCOV_EXCL_BR_LINE - a failed lock needs an invalid mutex
      ::pthread_mutex_unlock(&mutex_);
    }
  }
  CacheLock(const CacheLock&) = delete;
  CacheLock& operator=(const CacheLock&) = delete;
  CacheLock(CacheLock&&) = delete;
  CacheLock& operator=(CacheLock&&) = delete;

  [[nodiscard]] bool acquired() const { return owned_; }
  /// True when the previous owner died inside the critical section: the data
  /// structure must be rebuilt before it is used again.
  [[nodiscard]] bool owner_died() const { return owner_died_; }

private:
  pthread_mutex_t& mutex_;
  bool owned_ = false;
  bool owner_died_ = false;
};

}  // namespace

std::size_t shared_cache_region_size(std::size_t capacity) {
  return sizeof(SharedCacheData) +
         clamp_capacity(capacity) * (sizeof(int32_t) + sizeof(SharedCacheEntry));
}

SharedGroupCache::~SharedGroupCache() {
  // Only the owner unmaps.  The process-shared mutex and condition variable
  // are deliberately *not* destroyed: a worker that unwinds normally must not
  // tear down the synchronization objects the whole pool uses, and the kernel
  // reclaims the anonymous mapping when the last process exits.
  if (data_ != nullptr && owns_mmap_) {
    ::munmap(data_, bytes_);
    data_ = nullptr;
    owns_mmap_ = false;
    bytes_ = 0;
  }
}

bool SharedGroupCache::create(std::size_t capacity) {
  if (data_ != nullptr) {
    return false;  // already created
  }

  const std::size_t slots = clamp_capacity(capacity);
  const std::size_t bytes = shared_cache_region_size(slots);

  void* region = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // GCOV_EXCL_START - mmap() only fails on address-space exhaustion
  if (region == MAP_FAILED) {
    return false;
  }
  // GCOV_EXCL_STOP

  // Placement-new the header to initialize all fields to zero.
  auto* d = new (region) SharedCacheData{};
  d->capacity = static_cast<uint32_t>(slots);

  // Initialize the process-shared, robust mutex.
  ::pthread_mutexattr_t mutex_attr;
  ::pthread_mutexattr_init(&mutex_attr);
  ::pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
  // Robust: if a process crashes while holding the lock, the others can
  // recover (EOWNERDEAD) instead of deadlocking forever.
  ::pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
  // GCOV_EXCL_START - pthread_mutex_init() only fails on resource exhaustion
  if (::pthread_mutex_init(&d->mutex, &mutex_attr) != 0) {
    ::pthread_mutexattr_destroy(&mutex_attr);
    ::munmap(region, bytes);
    return false;
  }
  // GCOV_EXCL_STOP
  ::pthread_mutexattr_destroy(&mutex_attr);

  // Initialize the process-shared condition variable (monotonic clock).
  ::pthread_condattr_t cond_attr;
  ::pthread_condattr_init(&cond_attr);
  ::pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
  ::pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  // GCOV_EXCL_START - pthread_cond_init() only fails on resource exhaustion
  if (::pthread_cond_init(&d->cond, &cond_attr) != 0) {
    ::pthread_condattr_destroy(&cond_attr);
    ::pthread_mutex_destroy(&d->mutex);
    ::munmap(region, bytes);
    return false;
  }
  // GCOV_EXCL_STOP
  ::pthread_condattr_destroy(&cond_attr);

  // Bucket heads: -1 = empty chain.
  int32_t* buckets = bucket_array(d);
  std::fill(buckets, buckets + slots, -1);

  data_ = d;
  owns_mmap_ = true;
  bytes_ = bytes;
  return true;
}

void SharedGroupCache::attach(SharedCacheData* data) {
  data_ = data;
  owns_mmap_ = false;
  bytes_ = 0;
}

// ---- Internal helpers (caller holds the mutex) ----

bool SharedGroupCache::valid_index(int32_t index) const {
  return index >= 0 && static_cast<std::uint32_t>(index) < data_->capacity;
}

int32_t SharedGroupCache::find_locked(uint16_t addr) const {
  int32_t index = bucket_array(data_)[bucket_index(data_, addr)];
  const SharedCacheEntry* entries = entry_array(data_);
  // Every chain index must address a slot, and a chain can never be longer
  // than the table.  Both bounds matter: without them a torn chain (see
  // rebuild_locked) reads outside the slot array -- shared memory that every
  // worker maps.
  for (std::uint32_t hops = 0; index >= 0 && hops < data_->capacity; ++hops) {
    if (!valid_index(index)) {
      return -1;  // corrupt chain: treat the address as absent
    }
    const SharedCacheEntry& entry = entries[index];
    if (entry.addr == addr) {
      return index;
    }
    index = entry.hash_next;
  }
  return -1;
}

void SharedGroupCache::rebuild_locked() const {
  const std::uint32_t capacity = data_->capacity;
  SharedCacheEntry* entries = entry_array(data_);
  int32_t* buckets = bucket_array(data_);
  std::fill(buckets, buckets + capacity, -1);
  data_->lru_head = -1;
  data_->lru_tail = -1;

  // A count beyond the capacity is itself a symptom of the crash: only the
  // addressable slots are kept.
  const std::uint32_t used = std::min(data_->num_entries, capacity);
  data_->num_entries = used;

  for (std::uint32_t slot = 0; slot < used; ++slot) {
    SharedCacheEntry& entry = entries[slot];
    // A torn write can leave a length larger than the value array; a read
    // would then copy past the slot into the neighbouring one.
    entry.value_len =
        static_cast<std::uint16_t>(std::min<std::size_t>(entry.value_len, kMaxSharedValueBytes));
    entry.hash_next = -1;
    // Re-link into its bucket chain.
    const std::size_t bucket = bucket_index(data_, entry.addr);
    entry.hash_next = buckets[bucket];
    buckets[bucket] = static_cast<int32_t>(slot);
    // Re-link at the front of the LRU list (link_front_locked() inlined: the
    // repair must also be reachable from the const read paths).
    entry.lru_prev = -1;
    entry.lru_next = data_->lru_head;
    if (data_->lru_head >= 0) {
      entries[data_->lru_head].lru_prev = static_cast<int32_t>(slot);
    }
    data_->lru_head = static_cast<int32_t>(slot);
    if (data_->lru_tail < 0) {
      data_->lru_tail = static_cast<int32_t>(slot);
    }
  }
}

int32_t SharedGroupCache::allocate_locked() {
  if (data_->num_entries < data_->capacity) {
    return static_cast<int32_t>(data_->num_entries++);
  }
  // Table full: recycle the least recently pushed address.
  int32_t victim = data_->lru_tail;
  if (!valid_index(victim)) {
    // The LRU list was torn.  rebuild_locked() re-links every slot, so the
    // tail below is valid again (the table is full, so it is not empty).
    rebuild_locked();
    victim = data_->lru_tail;
  }
  unlink_bucket_locked(victim);
  unlink_lru_locked(victim);
  ++data_->evictions;
  return victim;
}

void SharedGroupCache::unlink_bucket_locked(int32_t index) {
  // Precondition: index is a valid slot (allocate_locked() either returns a
  // free slot or rebuilds the table before choosing the victim).
  SharedCacheEntry* entries = entry_array(data_);
  int32_t* head = &bucket_array(data_)[bucket_index(data_, entries[index].addr)];
  int32_t* link = head;
  // The victim is always in its own chain, so this terminates for a healthy
  // table.  Bound the walk anyway: a corrupt chain must not loop forever.
  for (std::uint32_t hops = 0; *link != index; ++hops) {
    if (!valid_index(*link) || hops >= data_->capacity) {
      return;  // not in its chain: nothing to unlink
    }
    link = &entries[*link].hash_next;
  }
  *link = entries[index].hash_next;
}

void SharedGroupCache::unlink_lru_locked(int32_t index) {
  SharedCacheEntry* entries = entry_array(data_);
  const SharedCacheEntry& entry = entries[index];
  // A torn neighbour index (see rebuild_locked) must not be followed: it
  // would write outside the slot array, in memory shared by every worker.
  const int32_t prev = valid_index(entry.lru_prev) ? entry.lru_prev : -1;
  const int32_t next = valid_index(entry.lru_next) ? entry.lru_next : -1;
  if (prev >= 0) {
    entries[prev].lru_next = next;
  } else {
    data_->lru_head = next;
  }
  if (next >= 0) {
    entries[next].lru_prev = prev;
  } else {
    data_->lru_tail = prev;
  }
  entries[index].lru_prev = -1;
  entries[index].lru_next = -1;
}

void SharedGroupCache::link_front_locked(int32_t index) {
  SharedCacheEntry* entries = entry_array(data_);
  entries[index].lru_prev = -1;
  entries[index].lru_next = valid_index(data_->lru_head) ? data_->lru_head : -1;
  if (entries[index].lru_next >= 0) {
    entries[entries[index].lru_next].lru_prev = index;
  }
  data_->lru_head = index;
  if (data_->lru_tail < 0) {
    data_->lru_tail = index;
  }
}

void SharedGroupCache::touch_locked(int32_t index) {
  unlink_lru_locked(index);
  link_front_locked(index);
}

// ---- Public API ----

void SharedGroupCache::push(uint16_t addr, const std::vector<uint8_t>& value) {
  if (data_ == nullptr) {
    return;
  }

  {
    CacheLock lock(data_->mutex);
    // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
    if (!lock.acquired()) {
      return;
    }
    // GCOV_EXCL_STOP
    if (lock.owner_died()) {
      rebuild_locked();
    }

    // Increment the raw counter and store the wrapped position so the
    // client-facing index `i` stays bounded.  Wrapped values are 1-based
    // (1..kPositionModulus-1, 0 at wrap) so they match position() and the
    // client's `i` parameter: after a single push pushed_at == position().
    const uint32_t raw_pos = data_->position.fetch_add(1);
    const uint32_t wrapped_pos = (raw_pos + 1) % kPositionModulus;

    int32_t index = find_locked(addr);
    if (index < 0) {
      index = allocate_locked();
      SharedCacheEntry* entries = entry_array(data_);
      entries[index] = SharedCacheEntry{};
      entries[index].addr = addr;
      const std::size_t bucket = bucket_index(data_, addr);
      entries[index].hash_next = bucket_array(data_)[bucket];
      bucket_array(data_)[bucket] = index;
      link_front_locked(index);
    } else {
      touch_locked(index);
    }

    SharedCacheEntry& entry = entry_array(data_)[index];
    const std::size_t len = std::min(value.size(), kMaxSharedValueBytes);
    entry.value_len = static_cast<uint16_t>(len);
    entry.timestamp = static_cast<uint32_t>(std::time(nullptr));
    entry.pushed_at = wrapped_pos;
    std::memset(entry.value, 0, sizeof(entry.value));
    if (len > 0) {
      std::memcpy(entry.value, value.data(), len);
    }

    // Signal new data.
    data_->generation.fetch_add(1);
  }

  // Wake all blocked long-poll readers (outside the lock).
  notify_all();
}

std::optional<std::vector<uint8_t>> SharedGroupCache::get(uint16_t addr, int max_age_sec) const {
  if (data_ == nullptr) {
    return std::nullopt;
  }

  CacheLock lock(data_->mutex);
  // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return std::nullopt;
  }
  // GCOV_EXCL_STOP
  if (lock.owner_died()) {
    rebuild_locked();
  }

  std::optional<std::vector<uint8_t>> result;
  const int32_t index = find_locked(addr);
  if (index >= 0) {
    const SharedCacheEntry& entry = entry_array(data_)[index];
    // Signed difference: a clock step backwards must neither wrap nor make
    // the entry look infinitely old.
    const std::int64_t age =
        static_cast<std::int64_t>(std::time(nullptr)) - static_cast<std::int64_t>(entry.timestamp);
    if (max_age_sec < 0 || age < static_cast<std::int64_t>(max_age_sec)) {
      // Clamp: a torn length must never make the copy run past the slot.
      const std::size_t len = std::min<std::size_t>(entry.value_len, kMaxSharedValueBytes);
      result.emplace(entry.value, entry.value + len);
    }
  }
  return result;
}

SharedGroupCache::Delta SharedGroupCache::get_delta(uint32_t since_pos,
                                                    const std::set<uint16_t>& subscribed,
                                                    int max_age_sec) const {
  Delta delta;
  if (data_ == nullptr) {
    return delta;
  }

  CacheLock lock(data_->mutex);
  // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return delta;
  }
  // GCOV_EXCL_STOP
  if (lock.owner_died()) {
    rebuild_locked();
  }

  // Current wrapped position (0-based, wraps at kPositionModulus).
  delta.position = data_->position.load() % kPositionModulus;

  // Modular distance from the client's last-seen position to the current position.
  const uint32_t epoch_distance =
      (delta.position - since_pos + kPositionModulus) % kPositionModulus;

  // Epoch detection: a client that is more than half the modulus behind comes
  // from a previous wrap-around epoch and gets a full refresh.
  // With M=100_000 this threshold is 50_000 entries (~17 min at 50 tps), far
  // exceeding the 300-second long-poll timeout.
  const bool epoch_mismatch = (since_pos != 0 && epoch_distance >= kPositionModulus / 2);

  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

  for (const uint16_t addr : subscribed) {
    const int32_t index = find_locked(addr);
    if (index < 0) {
      continue;
    }
    const SharedCacheEntry& entry = entry_array(data_)[index];
    const std::int64_t age = now - static_cast<std::int64_t>(entry.timestamp);
    if (max_age_sec >= 0 && age >= static_cast<std::int64_t>(max_age_sec)) {
      continue;
    }

    if (!epoch_mismatch) {
      const uint32_t entry_dist =
          (entry.pushed_at - since_pos + kPositionModulus) % kPositionModulus;
      // An entry is newer if its distance is positive and within epoch_distance.
      // When since_pos=0 (new client), pushed_at=0 is valid first-push data.
      if (entry_dist > epoch_distance) {
        continue;
      }
      if (since_pos != 0 && entry_dist == 0) {
        continue;
      }
    }
    // Clamp: a torn length must never make the copy run past the slot.
    const std::size_t len = std::min<std::size_t>(entry.value_len, kMaxSharedValueBytes);
    delta.values[addr] = std::vector<uint8_t>(entry.value, entry.value + len);
  }
  return delta;
}

bool SharedGroupCache::wait_for_new_data(int timeout_ms) {
  if (data_ == nullptr) {
    return false;
  }

  CacheLock lock(data_->mutex);
  // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return false;
  }
  // GCOV_EXCL_STOP

  const uint32_t gen_before = data_->generation.load();
  if (timeout_ms <= 0) {
    // Nothing can arrive within a zero-length wait.
    return false;
  }

  // Absolute deadline on the monotonic clock (the condvar uses it too).
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  const int64_t total_ns =
      static_cast<int64_t>(ts.tv_nsec) + static_cast<int64_t>(timeout_ms) * 1'000'000LL;
  ts.tv_sec += static_cast<time_t>(total_ns / 1'000'000'000LL);
  ts.tv_nsec = static_cast<decltype(ts.tv_nsec)>(total_ns % 1'000'000'000LL);

  // Wait loop: handles spurious wakeups.
  bool timed_out = false;
  while (data_->generation.load() == gen_before && !timed_out) {
    const int rc = ::pthread_cond_timedwait(&data_->cond, &data_->mutex, &ts);
    if (rc == ETIMEDOUT) {
      timed_out = true;
    }
    // GCOV_EXCL_START - a hard condvar error needs an invalid condvar
    else if (rc != 0) {
      break;
    }
    // GCOV_EXCL_STOP
  }

  return data_->generation.load() != gen_before;
}

void SharedGroupCache::notify_all() {
  if (data_ != nullptr) {
    // No need to hold the mutex for broadcast (POSIX allows it).
    ::pthread_cond_broadcast(&data_->cond);
  }
}

std::size_t SharedGroupCache::size() const {
  if (data_ == nullptr) {
    return 0;
  }
  CacheLock lock(data_->mutex);
  // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return 0;
  }
  // GCOV_EXCL_STOP
  return data_->num_entries;
}

std::size_t SharedGroupCache::capacity() const {
  if (data_ == nullptr) {
    return 0;
  }
  return data_->capacity;
}

std::uint64_t SharedGroupCache::evictions() const {
  if (data_ == nullptr) {
    return 0;
  }
  CacheLock lock(data_->mutex);
  // GCOV_EXCL_START - see CacheLock: a hard lock error needs an invalid mutex
  if (!lock.acquired()) {
    return 0;
  }
  // GCOV_EXCL_STOP
  return data_->evictions;
}

void SharedGroupCache::publish_bus_status(bool available) {
  publish_bus_status(available, static_cast<uint32_t>(std::time(nullptr)));
}

void SharedGroupCache::publish_bus_status(bool available, uint32_t now) {
  if (data_ == nullptr) {
    return;
  }

  if (available) {
    // Timestamps first, then the flag: a reader that sees "available" with
    // acquire ordering also sees the timestamps.
    data_->bus_last_ok.store(now);
    data_->bus_down_since.store(0);
    data_->bus_available.store(1, std::memory_order_release);
    return;
  }

  // Keep the timestamp of the first failure of the current outage — retries
  // every few seconds must not move the reported "down since" forward.
  uint32_t expected = 0;
  (void)data_->bus_down_since.compare_exchange_strong(expected, now);
  // Publish "unavailable" only after down_since is meaningful, so a reader can
  // never observe an outage without a "since" timestamp.
  data_->bus_available.store(0, std::memory_order_release);
}

SharedGroupCache::BusStatus SharedGroupCache::bus_status() const {
  BusStatus status;
  if (data_ == nullptr) {
    return status;
  }
  // Load the flag first (acquire): it publishes the timestamps written before.
  status.available = data_->bus_available.load(std::memory_order_acquire) != 0;
  status.last_ok = data_->bus_last_ok.load();
  status.down_since = data_->bus_down_since.load();
  return status;
}

}  // namespace cvknx
