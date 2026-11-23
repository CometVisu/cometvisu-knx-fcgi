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
 * @file shared_group_cache.h
 * @brief Shared-memory KNX group telegram cache with process-shared synchronization.
 *
 * Lives in mmap(MAP_SHARED|MAP_ANONYMOUS) memory so all worker processes
 * see the same data.  A dedicated cache reader process drains the single
 * bus connection and pushes telegrams here.  Worker processes query the
 * cache directly — no per-process bus connection needed.
 *
 * Synchronization:
 *   - pthread_mutex_t (PTHREAD_PROCESS_SHARED, robust) protects all entries
 *   - pthread_cond_t (PTHREAD_PROCESS_SHARED) wakes blocked long-poll readers
 *   - std::atomic<uint32_t> position   — monotonically increasing, shared
 *   - std::atomic<uint32_t> generation — lets waiters detect new data
 *
 * Storage
 * -------
 * The cache holds **the most recently pushed `capacity` distinct group
 * addresses** and evicts the least recently pushed one when a new address
 * arrives (LRU).  It is a fixed-size chained hash table inside the mmap'd
 * region: `capacity` bucket heads followed by `capacity` entry slots, plus an
 * intrusive LRU list.  No heap allocation, no tombstone degradation, and
 * eviction is O(1).
 *
 * `capacity` is a runtime value (KNX_CACHE_CAPACITY): the region is sized
 * once in the parent process before fork(), so every worker sees the same
 * table.
 *
 * Group address 0 is a valid address and is stored like any other; the
 * occupancy of a slot is defined by the bucket chains, not by a sentinel.
 */

#pragma once

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <set>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace cvknx {

/// Default number of distinct group addresses kept in the cache.
/// 16384 entries are ~700 KiB of shared memory — enough for the largest
/// CometVisu installations with a wide margin.
inline constexpr std::size_t kDefaultCacheCapacity = 16384;

/// Smallest accepted capacity (tests use tiny values to exercise eviction).
inline constexpr std::size_t kMinCacheCapacity = 4;

/// Largest accepted capacity (guards against a nonsense environment value).
inline constexpr std::size_t kMaxCacheCapacity = 1'000'000;

/// Position modulus — the index `i` wraps at this value to keep the per-request
/// transmitted bytes small while covering at least 24 hours of full-speed KNX bus
/// traffic (~50 telegrams/sec).  At 50 tps this gives ~55 hours before the half-range
/// ambiguity kicks in; at 100 tps ~28 hours.  The half-range rule means a client
/// that polls at least once every M/2 entries will always get correct deltas.
/// With the 300-second long-poll timeout this is easily satisfied.
inline constexpr uint32_t kPositionModulus = 100'000;

/// Maximum KNX APDU payload bytes storable per entry.
inline constexpr std::size_t kMaxSharedValueBytes = 14;

/// One cache entry — must be trivially copyable (POD) for shared memory.
struct SharedCacheEntry {
  uint16_t addr = 0;                         // KNX group address (16-bit EIB)
  uint16_t value_len = 0;                    // actual value bytes used
  uint32_t timestamp = 0;                    // Unix epoch seconds
  uint32_t pushed_at = 0;                    // cache position when pushed
  int32_t hash_next = -1;                    // next slot in the bucket chain
  int32_t lru_prev = -1;                     // more recently pushed neighbour
  int32_t lru_next = -1;                     // less recently pushed neighbour
  uint8_t value[kMaxSharedValueBytes] = {};  // NOLINT(modernize-avoid-c-arrays)
};
static_assert(std::is_trivially_copyable_v<SharedCacheEntry>,
              "SharedCacheEntry must be POD for shared memory");

/// The fixed part of the shared-memory region.  The bucket heads
/// (`capacity` × int32) and the entry slots (`capacity` × SharedCacheEntry)
/// follow it — see region_size().
struct SharedCacheData {
  alignas(64) pthread_mutex_t mutex;
  alignas(64) pthread_cond_t cond;
  alignas(64) std::atomic<uint32_t> position{0};
  // Separate cache line: the reader increments it on every telegram while
  // every long-poll waiter loads it.
  alignas(64) std::atomic<uint32_t> generation{0};
  // Bus availability, published by the cache reader process.  Workers read it
  // to tell /r and /w clients that the bus connection is gone.  Starts as
  // "available": until the reader reports otherwise we must not reject reads
  // (the reader publishes its real state right after it starts).
  alignas(64) std::atomic<uint32_t> bus_available{1};  // 1 = reader connected
  std::atomic<uint32_t> bus_last_ok{0};                // last success (0 = never)
  std::atomic<uint32_t> bus_down_since{0};             // outage start (0 = up)
  uint32_t capacity = 0;                               // slots in this region
  uint32_t num_entries = 0;                            // slots in use
  int32_t lru_head = -1;                               // most recently pushed
  int32_t lru_tail = -1;                               // least recently pushed
  uint64_t evictions = 0;                              // lifetime counter
};

/// Size of the shared region for @p capacity distinct addresses.
[[nodiscard]] std::size_t shared_cache_region_size(std::size_t capacity);

/**
 * @brief Shared-memory group cache accessible by all worker processes.
 *
 * Wraps a SharedCacheData region in mmap'd memory.  The creator calls
 * create() which mmaps and initializes the mutex/condvar.  Other processes
 * call attach() with the existing data pointer.
 *
 * Thread/process safety:
 *   - All entry access is protected by the process-shared mutex
 *   - wait_for_new_data() blocks on the process-shared condition variable
 *   - position() is lock-free (atomic read)
 */
class SharedGroupCache {
public:
  SharedGroupCache() = default;
  ~SharedGroupCache();

  // Non-copyable, non-movable (owns external mmap reference).
  SharedGroupCache(const SharedGroupCache&) = delete;
  SharedGroupCache& operator=(const SharedGroupCache&) = delete;
  SharedGroupCache(SharedGroupCache&&) = delete;
  SharedGroupCache& operator=(SharedGroupCache&&) = delete;

  /// Create the shared memory region and initialize synchronization primitives.
  /// Must be called once by the parent process before fork().
  /// @param capacity number of distinct addresses kept (LRU eviction beyond).
  /// @return true on success.
  [[nodiscard]] bool create(std::size_t capacity = kDefaultCacheCapacity);

  /// Attach to an already-created shared data region (for tests or child processes).
  /// @param data Pointer to initialized SharedCacheData.
  void attach(SharedCacheData* data);

  /// Whether the cache is initialized (create() or attach() succeeded).
  [[nodiscard]] bool is_initialized() const { return data_ != nullptr; }

  /// Push a value.  Called by the cache reader process.
  /// Increments position and generation, broadcasts condition variable.
  /// When the table is full, the least recently pushed address is evicted.
  void push(uint16_t addr, const std::vector<uint8_t>& value);

  /// Get latest value for an address, optionally age-filtered.
  /// Called by worker processes.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t addr, int max_age_sec = -1) const;

  /// Entries newer than since_pos for subscribed addresses.
  struct Delta {
    std::unordered_map<uint16_t, std::vector<uint8_t>> values;
    uint32_t position = 0;
  };
  /// Only entries with pushed_at newer than since_pos are returned (modular comparison).
  /// If the client is from a previous epoch (modular distance ≥ kPositionModulus / 2),
  /// all current values for subscribed addresses are returned instead (full refresh).
  /// Called by worker processes.
  [[nodiscard]] Delta get_delta(uint32_t since_pos, const std::set<uint16_t>& subscribed,
                                int max_age_sec = -1) const;

  /// Current position (lock-free, wraps at kPositionModulus).
  /// Returns the pushed_at of the most recently pushed entry (1-based),
  /// or 0 if no entries have been pushed yet.
  [[nodiscard]] uint32_t position() const {
    return data_ == nullptr ? 0 : data_->position.load() % kPositionModulus;
  }

  /// Block until new data arrives or the timeout expires.
  /// Uses the process-shared condition variable — zero CPU while waiting.
  /// @param timeout_ms Maximum wait time in milliseconds; 0 or less means
  ///        "do not block" and always reports false (nothing can arrive in
  ///        zero time).
  /// @return true if new data arrived, false on timeout.
  [[nodiscard]] bool wait_for_new_data(int timeout_ms);

  /// Broadcast to all waiters that new data is available.
  /// Called by the cache reader after push().
  void notify_all();

  /// Get the raw data pointer (for passing to child processes).
  [[nodiscard]] SharedCacheData* data() { return data_; }

  /// Number of addresses currently stored.
  [[nodiscard]] std::size_t size() const;

  /// Capacity of the region.
  [[nodiscard]] std::size_t capacity() const;

  /// Number of addresses dropped because the table was full (LRU evictions).
  [[nodiscard]] std::uint64_t evictions() const;

  /// knxd availability as published by the cache reader process.
  struct BusStatus {
    bool available = true;    ///< cache reader holds a bus connection (false once reported)
    uint32_t last_ok = 0;     ///< Unix time (s) of the last successful connection, 0 = never
    uint32_t down_since = 0;  ///< Unix time (s) the connection broke, 0 = up
  };

  /// Publish the cache reader's bus connection state.  Called by the cache
  /// reader process only.  Every failure keeps the timestamp of the first
  /// failure of the current outage, so clients can report "down since X".
  void publish_bus_status(bool available);

  /// Same, with an explicit timestamp (deterministic tests).
  void publish_bus_status(bool available, uint32_t now);

  /// Read the published bus state (lock-free, acquire/release ordered).
  [[nodiscard]] BusStatus bus_status() const;

  /// Size of the shared memory region in bytes.
  [[nodiscard]] static std::size_t region_size(std::size_t capacity = kDefaultCacheCapacity) {
    return shared_cache_region_size(capacity);
  }

private:
  SharedCacheData* data_ = nullptr;
  bool owns_mmap_ = false;  // true if we created (and must unmap) the region
  std::size_t bytes_ = 0;   // size of the region we created

  /// Slot index holding @p addr, or -1.
  [[nodiscard]] int32_t find_locked(uint16_t addr) const;
  /// True when @p index addresses a slot of the table.
  [[nodiscard]] bool valid_index(int32_t index) const;
  /// Rebuild every bucket chain and the LRU list from the slot array.
  ///
  /// A process that dies while holding the robust mutex can leave the table
  /// half-updated (a chain pointing at a recycled slot, a dangling LRU tail,
  /// a length field from a torn write).  Dropping the cached values by
  /// rebuilding the structure is always better than traversing a corrupt
  /// chain: the cache is repopulated by the next telegrams, a corrupt table
  /// would crash or wedge every worker.
  ///
  /// `const` only because the read paths (get/get_delta) must repair too — it
  /// mutates the shared region behind `data_`, not this object.
  void rebuild_locked() const;
  /// A free slot: recycles the LRU entry when the table is full.
  [[nodiscard]] int32_t allocate_locked();
  /// Remove @p index from its bucket chain.
  void unlink_bucket_locked(int32_t index);
  /// Remove @p index from the LRU list.
  void unlink_lru_locked(int32_t index);
  /// Insert @p index at the front (most recently used) of the LRU list.
  /// The slot must not be linked yet.
  void link_front_locked(int32_t index);
  /// Move @p index to the front (most recently used) of the LRU list.
  void touch_locked(int32_t index);
};

}  // namespace cvknx
