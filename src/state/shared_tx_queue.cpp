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
 * @file shared_tx_queue.cpp
 * @brief Shared TX ring implementation.
 */

#include "state/shared_tx_queue.h"

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

namespace cvknx {

SharedTxQueue::~SharedTxQueue() {
  if (eventfd_ >= 0 && owns_eventfd_) {  // GCOV_EXCL_BR_LINE - fd and ownership are set together
    ::close(eventfd_);
    eventfd_ = -1;
  }
  if (data_ != nullptr && owns_mmap_) {
    // The process-shared mutex is deliberately not destroyed: a worker that
    // unwinds normally must not tear down the object the whole pool uses.
    ::munmap(data_, sizeof(SharedTxQueueData));
    data_ = nullptr;
    owns_mmap_ = false;
  }
}

bool SharedTxQueue::create() {
  if (data_ != nullptr) {
    return false;  // already created
  }

  void* region = ::mmap(nullptr, sizeof(SharedTxQueueData), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // GCOV_EXCL_START - allocation failures cannot be injected portably
  if (region == MAP_FAILED) {
    return false;
  }
  // GCOV_EXCL_STOP

  auto* data = new (region) SharedTxQueueData{};

  ::pthread_mutexattr_t attr;
  ::pthread_mutexattr_init(&attr);
  ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  // Robust: a worker that dies while holding the lock must not wedge the queue.
  ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  // GCOV_EXCL_START - pthread_mutex_init only fails on resource exhaustion
  if (::pthread_mutex_init(&data->mutex, &attr) != 0) {
    ::pthread_mutexattr_destroy(&attr);
    ::munmap(region, sizeof(SharedTxQueueData));
    return false;
  }
  // GCOV_EXCL_STOP
  ::pthread_mutexattr_destroy(&attr);

  // Created before fork(): every child inherits the same descriptor, so a
  // worker's write() wakes the cache reader's poll().
  eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  // GCOV_EXCL_START - eventfd() only fails on fd exhaustion
  if (eventfd_ < 0) {
    ::pthread_mutex_destroy(&data->mutex);
    ::munmap(region, sizeof(SharedTxQueueData));
    return false;
  }
  // GCOV_EXCL_STOP

  owns_eventfd_ = true;
  data_ = data;
  owns_mmap_ = true;
  return true;
}

void SharedTxQueue::attach(SharedTxQueueData* data) {
  data_ = data;
  owns_mmap_ = false;
  eventfd_ = -1;
  owns_eventfd_ = false;
}

namespace {

/// Lock helper that recovers a robust mutex whose owner died (EOWNERDEAD).
///
/// `acquired()` is false only for an unrecoverable error, and the destructor
/// then leaves the mutex alone — unlocking a mutex you do not own is undefined
/// behaviour.
class QueueLock {
public:
  explicit QueueLock(pthread_mutex_t& mutex) : mutex_(mutex) {
    const int rc = ::pthread_mutex_lock(&mutex_);
    owned_ = (rc == 0);
    if (rc == EOWNERDEAD) {
      // The previous owner died mid-operation.  The ring indices are POD and
      // updated under the lock, so the state is still consistent.
      ::pthread_mutex_consistent(&mutex_);
      owned_ = true;
    }
  }
  ~QueueLock() {
    if (owned_) {  // GCOV_EXCL_BR_LINE - a failed lock needs an invalid mutex
      ::pthread_mutex_unlock(&mutex_);
    }
  }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;
  QueueLock(QueueLock&&) = delete;
  QueueLock& operator=(QueueLock&&) = delete;

  [[nodiscard]] bool acquired() const { return owned_; }

private:
  pthread_mutex_t& mutex_;
  bool owned_ = false;
};

}  // namespace

bool SharedTxQueue::push(std::uint16_t group_addr, const std::vector<std::uint8_t>& apdu) {
  if (data_ == nullptr || apdu.empty() || apdu.size() > kMaxTxValueBytes) {
    return false;
  }
  QueueLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs a corrupted mutex
  if (!lock.acquired()) {
    return false;  // never mutate the ring without the lock
  }
  // GCOV_EXCL_STOP
  const std::uint32_t next = (data_->tail + 1) % kTxQueueCapacity;
  if (next == data_->head) {
    ++data_->dropped;
    return false;  // full
  }
  TxSlot& slot = data_->slots[data_->tail];
  slot.group_addr = group_addr;
  slot.len = static_cast<std::uint8_t>(apdu.size());
  std::memset(slot.data, 0, sizeof(slot.data));
  std::memcpy(slot.data, apdu.data(), apdu.size());
  data_->tail = next;
  ++data_->pushed;
  return true;
}

bool SharedTxQueue::pop(std::uint16_t& out_group_addr, std::vector<std::uint8_t>& out_apdu) {
  if (data_ == nullptr) {
    return false;
  }
  QueueLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs a corrupted mutex
  if (!lock.acquired()) {
    return false;
  }
  // GCOV_EXCL_STOP
  if (data_->head == data_->tail) {
    return false;
  }
  const TxSlot& slot = data_->slots[data_->head];
  out_group_addr = slot.group_addr;
  // Clamp: a torn length byte must not copy past the slot into its neighbour.
  out_apdu.assign(slot.data, slot.data + std::min<std::size_t>(slot.len, kMaxTxValueBytes));
  data_->head = (data_->head + 1) % kTxQueueCapacity;
  return true;
}

std::size_t SharedTxQueue::size() const {
  if (data_ == nullptr) {
    return 0;
  }
  QueueLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs a corrupted mutex
  if (!lock.acquired()) {
    return 0;
  }
  // GCOV_EXCL_STOP
  if (data_->tail >= data_->head) {
    return data_->tail - data_->head;
  }
  return kTxQueueCapacity - data_->head + data_->tail;
}

bool SharedTxQueue::peek(std::uint16_t& out_group_addr, std::vector<std::uint8_t>& out_apdu) const {
  if (data_ == nullptr) {
    return false;
  }
  QueueLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs a corrupted mutex
  if (!lock.acquired()) {
    return false;
  }
  // GCOV_EXCL_STOP
  if (data_->head == data_->tail) {
    return false;
  }
  const TxSlot& slot = data_->slots[data_->head];
  out_group_addr = slot.group_addr;
  // Clamp: a torn length byte must not copy past the slot into its neighbour.
  out_apdu.assign(slot.data, slot.data + std::min<std::size_t>(slot.len, kMaxTxValueBytes));
  return true;
}

void SharedTxQueue::pop_drop() {
  if (data_ == nullptr) {
    return;
  }
  QueueLock lock(data_->mutex);
  // GCOV_EXCL_START - a hard lock error needs a corrupted mutex
  if (!lock.acquired()) {
    return;
  }
  // GCOV_EXCL_STOP
  if (data_->head != data_->tail) {
    data_->head = (data_->head + 1) % kTxQueueCapacity;
  }
}

void SharedTxQueue::signal() {
  if (eventfd_ < 0) {
    return;
  }
  const std::uint64_t one = 1;
  // A full eventfd counter (EAGAIN) means a wakeup is already pending.
  const ssize_t written = ::write(eventfd_, &one, sizeof(one));
  (void)written;
}

void SharedTxQueue::drain_notifications() {
  if (eventfd_ < 0) {
    return;
  }
  std::uint64_t value = 0;
  while (::read(eventfd_, &value, sizeof(value)) > 0) {
    // Keep reading until the counter is zero (EAGAIN).
  }
}

}  // namespace cvknx
