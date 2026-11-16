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
 * @file shared_tx_queue.h
 * @brief Shared TX ring between the worker processes and the cache reader.
 *
 * Exactly one process owns the bus connection (the cache reader).  Workers
 * only enqueue group telegrams here and signal an inherited eventfd; the
 * reader drains the queue and sends through its BusLink.  This is what makes
 * `FCGI_WORKERS` independent of the device limits (one knxd client slot, one
 * KNXnet/IP tunnel instead of one per worker).
 *
 * The ring lives in mmap(MAP_SHARED|MAP_ANONYMOUS) memory, protected by a
 * PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST mutex (same pattern as
 * SharedGroupCache).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cvknx {

/// Ring capacity (telegrams in flight).
inline constexpr std::size_t kTxQueueCapacity = 256;
/// Maximum APDU bytes per telegram.
inline constexpr std::size_t kMaxTxValueBytes = 14;

/// One queued group telegram — must be trivially copyable (shared memory).
struct TxSlot {
  std::uint16_t group_addr = 0;
  std::uint8_t len = 0;
  std::uint8_t data[kMaxTxValueBytes] = {};  // NOLINT(modernize-avoid-c-arrays)
};
static_assert(std::is_trivially_copyable_v<TxSlot>, "TxSlot must be POD");

/// The shared-memory region.
struct SharedTxQueueData {
  alignas(64) pthread_mutex_t mutex;
  std::uint32_t head = 0;            ///< read index
  std::uint32_t tail = 0;            ///< write index
  std::uint64_t pushed = 0;          ///< lifetime counter (diagnostics/tests)
  std::uint64_t dropped = 0;         ///< lifetime rejections (full queue)
  TxSlot slots[kTxQueueCapacity]{};  // NOLINT(modernize-avoid-c-arrays)
};

/**
 * @brief Multi-process FIFO of outgoing group telegrams.
 *
 * The creator calls create() before fork(); children inherit the pointer and
 * the eventfd.  attach() exists for tests that simulate a second process.
 */
class SharedTxQueue {
public:
  SharedTxQueue() = default;
  ~SharedTxQueue();

  SharedTxQueue(const SharedTxQueue&) = delete;
  SharedTxQueue& operator=(const SharedTxQueue&) = delete;
  SharedTxQueue(SharedTxQueue&&) = delete;
  SharedTxQueue& operator=(SharedTxQueue&&) = delete;

  /// Allocate the region and create the eventfd (must be called in the parent
  /// before fork so every child inherits the same fd).
  [[nodiscard]] bool create();

  /// Attach to an existing region (tests, child processes).
  void attach(SharedTxQueueData* data);

  [[nodiscard]] bool is_initialized() const { return data_ != nullptr; }

  /// Enqueue a telegram.
  /// @return false when the payload is too long, the queue is full or the
  ///         queue is not initialized.  A rejected telegram is *not* queued.
  [[nodiscard]] bool push(std::uint16_t group_addr, const std::vector<std::uint8_t>& apdu);

  /// Dequeue one telegram (FIFO).
  /// @return false when the queue is empty.
  [[nodiscard]] bool pop(std::uint16_t& out_group_addr, std::vector<std::uint8_t>& out_apdu);

  /// Look at the oldest telegram without removing it.
  /// Combined with pop_drop() this lets the sender keep a telegram queued when
  /// the bus link fails (never lose a write, never report a false success).
  [[nodiscard]] bool peek(std::uint16_t& out_group_addr, std::vector<std::uint8_t>& out_apdu) const;

  /// Remove the oldest telegram (the one last returned by peek()).
  void pop_drop();

  /// Number of queued telegrams.
  [[nodiscard]] std::size_t size() const;

  /// Wake the reader (writes to the inherited eventfd).  No-op without one.
  void signal();

  /// Consume pending eventfd notifications (must be called before draining,
  /// otherwise the reader would spin on a permanently readable fd).
  void drain_notifications();

  /// eventfd shared with the reader, or -1.
  [[nodiscard]] int eventfd() const { return eventfd_; }

  /// Raw region pointer (for passing to child processes).
  [[nodiscard]] SharedTxQueueData* data() { return data_; }

  [[nodiscard]] static std::size_t region_size() { return sizeof(SharedTxQueueData); }

private:
  SharedTxQueueData* data_ = nullptr;
  bool owns_mmap_ = false;
  int eventfd_ = -1;
  bool owns_eventfd_ = false;
};

}  // namespace cvknx
