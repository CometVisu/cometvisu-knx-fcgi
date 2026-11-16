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

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>

#include "state/shared_group_cache.h"

using namespace cvknx;

class SharedGroupCacheTest : public ::testing::Test {
public:
  void SetUp() override { ASSERT_TRUE(cache_.create()); }
  void TearDown() override {
    // Cache destructor handles munmap via owns_mmap_
  }

  SharedGroupCache cache_;
};

// ================================================================
// Basic push / get
// ================================================================

TEST_F(SharedGroupCacheTest, PushAndGet) {
  cache_.push(0x0A03, {0x42});  // 1/2/3
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 1);
  EXPECT_EQ((*val)[0], 0x42);
}

TEST_F(SharedGroupCacheTest, GetMissingReturnsNullopt) {
  auto val = cache_.get(0xFFFF);
  EXPECT_FALSE(val.has_value());
}

TEST_F(SharedGroupCacheTest, PushOverwritesExisting) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x99});
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x99);
}

TEST_F(SharedGroupCacheTest, MultiByteValue) {
  cache_.push(0x0B04, {0x0C, 0x6F});  // 2-byte temperature
  auto val = cache_.get(0x0B04);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 2);
  EXPECT_EQ((*val)[0], 0x0C);
  EXPECT_EQ((*val)[1], 0x6F);
}

// ================================================================
// Position monotonicity
// ================================================================

TEST_F(SharedGroupCacheTest, PositionMonotonic) {
  EXPECT_EQ(cache_.position(), 0);
  cache_.push(0x0A03, {0x01});
  EXPECT_EQ(cache_.position(), 1);
  cache_.push(0x0B04, {0x02});
  EXPECT_EQ(cache_.position(), 2);
  cache_.push(0x0C05, {0x03});
  EXPECT_EQ(cache_.position(), 3);
}

TEST_F(SharedGroupCacheTest, PositionNeverGoesBackward) {
  uint32_t prev = cache_.position();
  for (int i = 0; i < 100; ++i) {
    cache_.push(static_cast<uint16_t>(0x0A00 + i), {static_cast<uint8_t>(i)});
    uint32_t curr = cache_.position();
    EXPECT_GT(curr, prev) << "Position went backward at i=" << i;
    prev = curr;
  }
}

// ================================================================
// Position wrapping
// ================================================================

/// Position wraps at kPositionModulus and cycles 0..M-1..0.
TEST_F(SharedGroupCacheTest, PositionWrapsAtModulus) {
  // Push M-1 entries: position climbs to M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(0x0A03, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);  // max = 99,999

  // One more push wraps: position = M % M = 0.
  cache_.push(0x0A03, {0x42});
  EXPECT_EQ(cache_.position(), 0) << "Position should wrap to 0";

  // Continues advancing from 0.
  cache_.push(0x0A03, {0x99});
  EXPECT_EQ(cache_.position(), 1) << "Position advances after wrap";
}

/// Delta queries work correctly across the wrap boundary.
/// Client with i = M-2 receives packets at M-1, 0 (wrap), 1, 2 (current).
TEST_F(SharedGroupCacheTest, DeltaAcrossWrapBoundary) {
  uint16_t addr = 0x0A03;

  // Push M-1 entries: raw 0..M-2, pushed_at 0..M-2. position = M-1.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);

  // Push the wrap entry: raw = M-1, pushed_at = M-1. position = 0 (wrapped).
  cache_.push(addr, {0x99});

  // Push two more: positions 1 and 2.
  cache_.push(addr, {0xAA});
  cache_.push(addr, {0xBB});  // current value
  EXPECT_EQ(cache_.position(), 2);

  // Client with i = M-2 should see all entries from M-1 through 2.
  // Modular: since=M-2, current=2, epoch_distance=(2-(M-2)+M)%M=4.
  // Entries at pushed_at M-1, 0, 1, 2 → all with dist ≤ 4 → all match.
  // But same-address dedup gives only the latest (pushed_at=2 → 0xBB).
  auto delta = cache_.get_delta(kPositionModulus - 2, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0xBB) << "Must deliver latest post-wrap value";
  EXPECT_EQ(delta.position, 2) << "Position reflects current state";

  // Delta from the exact wrap position finds all subsequent entries.
  delta = cache_.get_delta(kPositionModulus - 1, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0xBB);
  EXPECT_EQ(delta.position, 2);
}

/// Epoch mismatch: client from early in the epoch, server near the end.
TEST_F(SharedGroupCacheTest, EpochMismatchTriggersFullRefresh) {
  uint16_t addr = 0x0A03;

  // Push M-3 entries (raw 0..M-4, pushed_at 0..M-4). position = M-3.
  for (uint32_t i = 0; i < kPositionModulus - 3; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }

  // Push current value: raw = M-3, pushed_at = M-3. position = M-2.
  cache_.push(addr, {0x42});

  // Client with i=10 (early epoch): epoch_distance = (M-2 - 10 + M) % M = M-12.
  // M-12 >= M/2 → epoch mismatch → full refresh.
  auto delta = cache_.get_delta(10, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0x42);
}

/// Position wraps 0→M-1→0→1 seamlessly (monotonic in raw).
TEST_F(SharedGroupCacheTest, PositionMonotonicAcrossWrap) {
  uint16_t addr = 0x0A03;

  // Push M-1 entries: position = M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);

  // One more push wraps: position = M % M = 0.
  cache_.push(addr, {0x42});
  EXPECT_EQ(cache_.position(), 0) << "Position wrapped to 0";

  // Pushing more advances again from the wrapped position.
  cache_.push(addr, {0x99});
  EXPECT_EQ(cache_.position(), 1) << "Position advances after wrap";
}

// ================================================================
// Delta queries
// ================================================================

TEST_F(SharedGroupCacheTest, GetDeltaReturnsNewerEntries) {
  cache_.push(0xFFFF, {0x00});  // dummy: pushed_at=1
  cache_.push(0x0A03, {0x01});  // pushed_at=2
  cache_.push(0x0B04, {0x02});  // pushed_at=3
  cache_.push(0x0C05, {0x03});  // pushed_at=4

  auto delta = cache_.get_delta(2, {0x0A03, 0x0B04, 0x0C05});
  // Only entries with pushed_at > 2 → pos 3 (0x0B04) and pos 4 (0x0C05)
  EXPECT_EQ(delta.values.size(), 2);
  EXPECT_TRUE(delta.values.contains(0x0B04));
  EXPECT_TRUE(delta.values.contains(0x0C05));
  EXPECT_FALSE(delta.values.contains(0x0A03));  // pushed_at=2, not > since_pos
  EXPECT_EQ(delta.position, 4);
}

TEST_F(SharedGroupCacheTest, GetDeltaEmptyWhenNoneNewer) {
  cache_.push(0x0A03, {0x01});  // pos 1

  auto delta = cache_.get_delta(5, {0x0A03});
  // since_pos (5) > current position (1) → epoch detection: effective_since = 0
  // pushed_at=1 > 0 → entry returned
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.position, 1);
}

/// Regression test: pushed_at and position() must be consistent.
/// After the first push, position() returns 1 but the entry had pushed_at=0
/// under the old code.  The client receives i=1, then polls with i=1.
/// The second push at pushed_at=1 was incorrectly excluded because
/// entry_dist==0 made it look like the same position.  This caused
/// the read handler to loop until timeout — "extreme delays".
TEST_F(SharedGroupCacheTest, PushedAtMatchesPositionForDeltaQuery) {
  uint16_t addr = 0x0A03;

  // Push first value — client receives this and gets position() as i.
  cache_.push(addr, {0x42});
  uint32_t pos_after_first = cache_.position();  // must match pushed_at semantics

  // Push second value — this is new data the client should receive.
  cache_.push(addr, {0x99});

  // Client polls with i=pos_after_first.  Must receive the second entry.
  auto delta = cache_.get_delta(pos_after_first, {addr});
  ASSERT_EQ(delta.values.size(), 1)
      << "Second push must be visible to client polling after first push";
  EXPECT_EQ(delta.values[addr][0], 0x99);
}

TEST_F(SharedGroupCacheTest, GetDeltaOnlySubscribed) {
  cache_.push(0x0A03, {0x01});  // pos 1
  cache_.push(0x0B04, {0x02});  // pos 2

  auto delta = cache_.get_delta(0, {0x0A03});  // only subscribed to 0x0A03
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_TRUE(delta.values.contains(0x0A03));
  EXPECT_FALSE(delta.values.contains(0x0B04));
}

// ================================================================
// Age filtering
// ================================================================

TEST_F(SharedGroupCacheTest, AgeFilterRejectsStaleEntries) {
  cache_.push(0x0A03, {0x42});
  // Age filter of 0 seconds — entry older than 0s is rejected.
  auto val = cache_.get(0x0A03, 0);
  EXPECT_FALSE(val.has_value()) << "Entry with timestamp=now should NOT be "
                                   "older than 0s (age filter rejects >= max_age)";

  // Actually: timestamp is `now` at push time. The age check is
  // `now - e.timestamp >= max_age_sec`.  With max_age_sec=0, this is
  // `now - now >= 0` → true → rejected.  This is correct behavior:
  // max_age_sec=0 means "only entries newer than 0 seconds" which is
  // impossible since time always advances by at least 1s between push
  // and get.
}

TEST_F(SharedGroupCacheTest, AgeFilterAcceptsFreshEntries) {
  cache_.push(0x0A03, {0x42});
  // A generous age filter must not hide a value that was just pushed.
  const auto val = cache_.get(0x0A03, 3600);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, (std::vector<uint8_t>{0x42}));
}

// A cache that was never created (or attached) must answer instead of crashing:
// every accessor is defensive about it.
TEST(SharedCacheDefaultState, PositionOfAnUninitialisedCacheIsZero) {
  SharedGroupCache cache;
  EXPECT_EQ(cache.position(), 0U);
}

// ================================================================
// Notification (condition variable)
// ================================================================

TEST_F(SharedGroupCacheTest, WaitForNewDataWakesOnPush) {
  // Start with some data so generation > 0
  cache_.push(0x0A03, {0x01});

  std::atomic<bool> woken{false};

  std::thread waiter([this, &woken]() {
    // Wait for new data — should wake when push happens
    bool got_data = cache_.wait_for_new_data(5000);
    woken.store(got_data);
  });

  // Give waiter time to enter the wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Push new data — should broadcast and wake the waiter
  cache_.push(0x0B04, {0x02});

  waiter.join();
  EXPECT_TRUE(woken.load()) << "Waiter was not woken by push";
}

TEST_F(SharedGroupCacheTest, WaitForNewDataTimeout) {
  // No data pushed — wait should time out
  bool got_data = cache_.wait_for_new_data(50);
  EXPECT_FALSE(got_data) << "Should time out when no new data arrives";
}

TEST_F(SharedGroupCacheTest, WaitForNewDataWithZeroTimeoutNeverBlocks) {
  // A zero-length wait can never observe new data: the contract is
  // "do not block, report no new data".
  EXPECT_FALSE(cache_.wait_for_new_data(0));

  cache_.push(0x0A03, {0x01});

  // Still false — the data is not *new* to a zero-length wait.
  EXPECT_FALSE(cache_.wait_for_new_data(0));
  EXPECT_FALSE(cache_.wait_for_new_data(-1));
}

// ================================================================
// Multiple cache instances sharing the same data (multi-worker)
// ================================================================

TEST_F(SharedGroupCacheTest, TwoInstancesShareSameData) {
  // Simulate two worker processes by attaching a second SharedGroupCache
  // to the same underlying SharedCacheData.
  SharedGroupCache cache2;
  cache2.attach(cache_.data());

  // Push via cache1
  cache_.push(0x0A03, {0x42});

  // Read via cache2 — should see the value
  auto val = cache2.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);

  // Position is shared
  EXPECT_EQ(cache2.position(), 1);
  EXPECT_EQ(cache_.position(), 1);

  // Push via cache2
  cache2.push(0x0B04, {0x99});

  // Read via cache1
  auto val2 = cache_.get(0x0B04);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ((*val2)[0], 0x99);

  // Position advanced consistently
  EXPECT_EQ(cache_.position(), 2);
  EXPECT_EQ(cache2.position(), 2);
}

// ================================================================
// Non-monotonic i bug reproduction
// ================================================================

/// This test verifies that two "worker" caches sharing the same data
/// always see a monotonically increasing position — the core bug fix.
TEST_F(SharedGroupCacheTest, MonotonicPositionAcrossTwoInstances) {
  SharedGroupCache cache2;
  cache2.attach(cache_.data());

  // Simulate worker A advancing the cache
  for (int i = 0; i < 10; ++i) {
    cache_.push(static_cast<uint16_t>(0x0A00 + i), {static_cast<uint8_t>(i)});
  }
  uint32_t pos_a = cache_.position();
  EXPECT_EQ(pos_a, 10);

  // Worker B queries — should see the SAME position (shared counter)
  uint32_t pos_b = cache2.position();
  EXPECT_EQ(pos_b, pos_a) << "BUG: position differs across cache instances";
  EXPECT_GE(pos_b, pos_a) << "BUG: position went backward across instances";
}

// ================================================================
// Large number of entries (fill the table)
// ================================================================

TEST_F(SharedGroupCacheTest, ManyEntries) {
  // Push many entries — should not crash or corrupt
  for (int i = 0; i < 500; ++i) {
    uint16_t addr = static_cast<uint16_t>(0x1000 + i);
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }

  // Verify some entries
  for (int i = 0; i < 500; ++i) {
    uint16_t addr = static_cast<uint16_t>(0x1000 + i);
    auto val = cache_.get(addr);
    ASSERT_TRUE(val.has_value()) << "Missing entry for addr " << i;
    EXPECT_EQ((*val)[0], static_cast<uint8_t>(i & 0xFF));
  }

  EXPECT_EQ(cache_.position(), 500);
}

// ================================================================
// max_age_sec = -1 (no age filter) — the default
// ================================================================

/// get() with max_age_sec=-1 must always return the cached entry regardless
/// of age.  This is the default and the semantically correct behavior for
/// initial reads — the caller wants the latest known value, not a value
/// filtered by an arbitrary time window.
TEST_F(SharedGroupCacheTest, GetWithNoAgeFilterReturnsData) {
  cache_.push(0x0A03, {0x42});
  auto val = cache_.get(0x0A03, -1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);
}

/// Contrast: get() with max_age_sec=0 rejects everything (timestamp never
/// strictly less than 0 seconds old).  get() with max_age_sec=-1 accepts
/// everything.  This is why the ReadHandler must pass -1, not timeout_sec.
TEST_F(SharedGroupCacheTest, GetAgeZeroVsNoFilter) {
  cache_.push(0x0A03, {0x42});

  // max_age_sec=0: entry is "now - now >= 0" → rejected.
  auto val_zero = cache_.get(0x0A03, 0);
  EXPECT_FALSE(val_zero.has_value())
      << "max_age_sec=0 must reject all entries (no entry can be <0s old)";

  // max_age_sec=-1: no age filter → accepted.
  auto val_no_filter = cache_.get(0x0A03, -1);
  ASSERT_TRUE(val_no_filter.has_value());
  EXPECT_EQ((*val_no_filter)[0], 0x42);
}

/// get_delta() with max_age_sec=-1 must filter ONLY by position
/// (pushed_at > since_pos), never by age.
TEST_F(SharedGroupCacheTest, GetDeltaWithNoAgeFilterReturnsByPositionOnly) {
  // With 1-based positions: pushed_at = 1, 2, 3, 4. position = 4.
  cache_.push(0x0A03, {0x01});  // pushed_at=1
  cache_.push(0x0A03, {0x02});  // pushed_at=2
  cache_.push(0x0A03, {0x03});  // pushed_at=3
  cache_.push(0x0A03, {0x04});  // pushed_at=4

  // since_pos=2: entries at pushed_at > 2 → pushed_at 3 and 4.
  // Same-address dedup gives only the latest (pushed_at=4 → 0x04).
  auto delta = cache_.get_delta(2, {0x0A03}, -1);
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_TRUE(delta.values.contains(0x0A03));
  EXPECT_EQ(delta.values[0x0A03][0], 0x04);
  EXPECT_EQ(delta.position, 4);

  // since_pos=3: entries at pushed_at > 3 → pushed_at=4 only.
  auto delta2 = cache_.get_delta(3, {0x0A03}, -1);
  EXPECT_EQ(delta2.values.size(), 1);
  EXPECT_EQ(delta2.values[0x0A03][0], 0x04);
  EXPECT_EQ(delta2.position, 4);

  // since_pos=4: entries at pushed_at > 4 → none.
  auto delta3 = cache_.get_delta(4, {0x0A03}, -1);
  EXPECT_TRUE(delta3.values.empty());
  EXPECT_EQ(delta3.position, 4);

  // since_pos=5: past end → epoch mismatch (since_pos > position, wraps).
  // Epoch mismatch triggers full refresh → all cached values returned.
  auto delta4 = cache_.get_delta(5, {0x0A03}, -1);
  EXPECT_EQ(delta4.values.size(), 1);
  EXPECT_EQ(delta4.values[0x0A03][0], 0x04);
  EXPECT_EQ(delta4.position, 4);
}

/// get_delta() with max_age_sec=0 must NOT return entries (same as get).
/// This demonstrates why passing timeout_sec as max_age_sec is dangerous:
/// with t=0 (timeout_sec=1), entries pushed 1+ seconds ago would be hidden.
TEST_F(SharedGroupCacheTest, GetDeltaWithAgeZeroRejectsAll) {
  cache_.push(0x0A03, {0x42});  // pushed_at=0, position=1

  // max_age_sec=0: age filter rejects everything.
  auto delta = cache_.get_delta(0, {0x0A03}, 0);
  EXPECT_TRUE(delta.values.empty()) << "max_age_sec=0 must reject all entries in get_delta";
  EXPECT_EQ(delta.position, 1);  // position is still reported

  // max_age_sec=-1: no age filter → entry returned.
  auto delta2 = cache_.get_delta(0, {0x0A03}, -1);
  EXPECT_EQ(delta2.values.size(), 1);
  EXPECT_TRUE(delta2.values.contains(0x0A03));
}

// ================================================================
// knxd availability (published by the cache reader process)
// ================================================================

TEST_F(SharedGroupCacheTest, BusStatusIsAssumedAvailableUntilReported) {
  // Before the cache reader reports anything we must not reject reads.
  const auto status = cache_.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_EQ(status.last_ok, 0U);
  EXPECT_EQ(status.down_since, 0U);
}

TEST_F(SharedGroupCacheTest, ConnectedKnxdReportsLastOk) {
  cache_.publish_bus_status(true, 1000);

  const auto status = cache_.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_EQ(status.last_ok, 1000U);
  EXPECT_EQ(status.down_since, 0U);
}

TEST_F(SharedGroupCacheTest, LosingTheConnectionRecordsTheOutageStart) {
  cache_.publish_bus_status(true, 1000);
  cache_.publish_bus_status(false, 1010);

  const auto status = cache_.bus_status();
  EXPECT_FALSE(status.available);
  EXPECT_EQ(status.last_ok, 1000U);     // when it last worked
  EXPECT_EQ(status.down_since, 1010U);  // when it broke
}

TEST_F(SharedGroupCacheTest, RepeatedFailuresKeepTheFirstOutageTimestamp) {
  cache_.publish_bus_status(true, 1000);
  cache_.publish_bus_status(false, 1010);
  cache_.publish_bus_status(false, 1999);  // still down after a retry

  EXPECT_EQ(cache_.bus_status().down_since, 1010U)
      << "the outage start must not move with every retry";
}

TEST_F(SharedGroupCacheTest, ReconnectingClearsTheOutage) {
  cache_.publish_bus_status(true, 1000);
  cache_.publish_bus_status(false, 1010);
  cache_.publish_bus_status(true, 2000);

  const auto status = cache_.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_EQ(status.last_ok, 2000U);
  EXPECT_EQ(status.down_since, 0U);
}

// ================================================================
// Lifecycle, degenerate state and recovery paths
// ================================================================

TEST(SharedCacheLifecycle, UninitialisedCacheIsInert) {
  SharedGroupCache cache;
  EXPECT_FALSE(cache.is_initialized());
  EXPECT_EQ(cache.data(), nullptr);

  // Every accessor must tolerate the "not created yet" state.
  cache.push(0x0A03, {0x42});
  EXPECT_FALSE(cache.get(0x0A03).has_value());
  const auto delta = cache.get_delta(0, {0x0A03});
  EXPECT_TRUE(delta.values.empty());
  EXPECT_EQ(delta.position, 0U);
  EXPECT_FALSE(cache.wait_for_new_data(1));
  cache.notify_all();
  cache.publish_bus_status(false);
  const auto status = cache.bus_status();
  EXPECT_TRUE(status.available);
  EXPECT_EQ(status.last_ok, 0U);
  EXPECT_EQ(status.down_since, 0U);
}

TEST(SharedCacheLifecycle, CreateIsIdempotent) {
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  EXPECT_TRUE(cache.is_initialized());
  EXPECT_FALSE(cache.create());  // already created
}

TEST(SharedCacheLifecycle, AttachedInstanceDoesNotUnmapTheRegion) {
  SharedGroupCache owner;
  ASSERT_TRUE(owner.create());
  {
    SharedGroupCache attached;
    attached.attach(owner.data());
    EXPECT_TRUE(attached.is_initialized());
    attached.push(0x0A03, {0x42});
    EXPECT_TRUE(attached.get(0x0A03).has_value());
  }  // attached must not unmap the shared region
  const auto value = owner.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ((*value)[0], 0x42);
}

class SmallCacheTest : public ::testing::Test {
public:
  void SetUp() override { ASSERT_TRUE(cache_.create(4)); }

  SharedGroupCache cache_;
};

/// The cache keeps the most recently pushed `capacity` addresses: pushing one
/// more address evicts the one that was pushed longest ago.
TEST_F(SmallCacheTest, EvictsTheLeastRecentlyPushedAddress) {
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A02, {0x02});
  cache_.push(0x0A03, {0x03});
  cache_.push(0x0A04, {0x04});
  EXPECT_EQ(cache_.size(), 4U);
  EXPECT_EQ(cache_.evictions(), 0U);

  // The fifth address displaces 0x0A01, the oldest one.
  cache_.push(0x0A05, {0x05});
  EXPECT_EQ(cache_.size(), 4U);
  EXPECT_EQ(cache_.evictions(), 1U);
  EXPECT_FALSE(cache_.get(0x0A01).has_value());
  EXPECT_TRUE(cache_.get(0x0A02).has_value());
  EXPECT_TRUE(cache_.get(0x0A05).has_value());
}

/// A repeated push is a *use*: the address moves to the front, so it survives
/// the next eviction.
TEST_F(SmallCacheTest, RepushingRefreshesRecency) {
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A02, {0x02});
  cache_.push(0x0A03, {0x03});
  cache_.push(0x0A04, {0x04});
  cache_.push(0x0A01, {0x11});  // 0x0A01 is now the most recent

  cache_.push(0x0A05, {0x05});  // evicts 0x0A02 instead of 0x0A01
  EXPECT_TRUE(cache_.get(0x0A01).has_value());
  EXPECT_FALSE(cache_.get(0x0A02).has_value());
}

/// Eviction reuses the victim's slot, so the table keeps exactly `capacity`
/// entries and every remaining address stays readable.
TEST_F(SmallCacheTest, EvictionKeepsTheTableConsistent) {
  for (std::uint16_t i = 0; i < 20; ++i) {
    cache_.push(static_cast<uint16_t>(0x1000 + i), {static_cast<std::uint8_t>(i)});
  }
  EXPECT_EQ(cache_.size(), 4U);
  EXPECT_EQ(cache_.evictions(), 16U);
  EXPECT_EQ(cache_.capacity(), 4U);
  for (std::uint16_t i = 16; i < 20; ++i) {
    const auto value = cache_.get(static_cast<uint16_t>(0x1000 + i));
    ASSERT_TRUE(value.has_value()) << "address " << i << " was evicted too early";
    EXPECT_EQ((*value)[0], static_cast<std::uint8_t>(i));
  }
  // A delta query for an evicted address finds nothing either.
  EXPECT_TRUE(cache_.get_delta(0, {0x1000}).values.empty());
}

/// Group address 0 is a real address, not the "empty slot" marker.
TEST_F(SmallCacheTest, StoresGroupAddressZero) {
  cache_.push(0x0000, {0x42});
  const auto value = cache_.get(0x0000);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ((*value)[0], 0x42);
  EXPECT_EQ(cache_.size(), 1U);
}

TEST(SharedCacheCapacity, DefaultCapacityIsLarge) {
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());
  EXPECT_EQ(cache.capacity(), kDefaultCacheCapacity);
  EXPECT_GE(kDefaultCacheCapacity, 16384U);
}

TEST(SharedCacheCapacity, CapacityIsClampedToTheSupportedRange) {
  SharedGroupCache tiny;
  ASSERT_TRUE(tiny.create(1));  // below the minimum
  EXPECT_EQ(tiny.capacity(), kMinCacheCapacity);

  SharedGroupCache none;  // uninitialised reports no capacity
  EXPECT_EQ(none.capacity(), 0U);
  EXPECT_EQ(none.size(), 0U);
  EXPECT_EQ(none.evictions(), 0U);
  EXPECT_EQ(none.region_size(64), shared_cache_region_size(64));
}

TEST_F(SharedGroupCacheTest, DeltaSkipsEntriesAheadOfTheCurrentPosition) {
  cache_.push(0x0A03, {0x42});
  // Simulate a wrap: the entry's pushed_at is now ahead of the position.
  cache_.data()->position.store(0);
  const auto delta = cache_.get_delta(0, {0x0A03});
  EXPECT_TRUE(delta.values.empty());
}

TEST_F(SharedGroupCacheTest, OwnerDeathIsRecoveredByEveryAccessor) {
  // A process that dies while holding the robust mutex leaves it inconsistent;
  // the next locker gets EOWNERDEAD and must make it consistent again.
  const auto kill_owner = [this]() {
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      ::pthread_mutex_lock(&cache_.data()->mutex);
      ::_Exit(0);  // never unlock, never flush coverage data
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  };

  kill_owner();
  cache_.push(0x0A03, {0x42});

  kill_owner();
  EXPECT_TRUE(cache_.get(0x0A03).has_value());

  kill_owner();
  const auto delta = cache_.get_delta(0, {0x0A03});
  EXPECT_EQ(delta.values.size(), 1U);

  kill_owner();
  (void)cache_.wait_for_new_data(0);
}

/// A push with no payload stores an empty value (the value length declares it);
/// the writer must not be handed a null pointer.
TEST_F(SmallCacheTest, StoresAnEmptyValue) {
  cache_.push(0x0A03, {});
  const auto value = cache_.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(value->empty());
}

/// Eviction must also work when the victim is not the head of its hash chain:
/// two addresses that share a bucket make the second the chain head while the
/// lru victim is the first.
TEST_F(SmallCacheTest, EvictsAnEntryInsideAHashChain) {
  // 0x0A01 and 0x0A05 share the bucket (capacity 4: addr % 4 == 1).
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A05, {0x05});
  cache_.push(0x0A02, {0x02});
  cache_.push(0x0A03, {0x03});

  cache_.push(0x0A04, {0x04});  // evicts 0x0A01, which sits behind 0x0A05
  EXPECT_FALSE(cache_.get(0x0A01).has_value());
  EXPECT_TRUE(cache_.get(0x0A05).has_value());
  EXPECT_TRUE(cache_.get(0x0A04).has_value());
}

// ================================================================
// Torn shared state (what a process that died mid-push leaves behind)
// ================================================================
//
// The table lives in memory shared by every worker, and data() is public, so
// these tests can reproduce exactly the state a crash between two updates of
// push() leaves behind.  Every accessor must fail safe — treat the address as
// absent and keep serving — instead of indexing outside the slot array.

namespace {

/// Mirror of the layout helpers in shared_group_cache.cpp.
int32_t* BucketHeads(SharedCacheData* data) {
  return reinterpret_cast<int32_t*>(data + 1);
}

SharedCacheEntry* Slots(SharedCacheData* data) {
  return reinterpret_cast<SharedCacheEntry*>(BucketHeads(data) + data->capacity);
}

}  // namespace

TEST_F(SmallCacheTest, AnOutOfRangeChainIndexIsTreatedAsAbsent) {
  cache_.push(0x0A03, {0x42});
  // A chain head outside the slot array: nothing may be dereferenced.
  BucketHeads(cache_.data())[0x0A03 % cache_.data()->capacity] = 4242;

  EXPECT_FALSE(cache_.get(0x0A03).has_value());
  EXPECT_FALSE(cache_.get(0x0A07).has_value());  // same bucket
  EXPECT_TRUE(cache_.get_delta(0, {0x0A03}).values.empty());
}

TEST_F(SmallCacheTest, ACyclicHashChainTerminates) {
  cache_.push(0x0A03, {0x42});
  Slots(cache_.data())[0].hash_next = 0;  // points at itself

  // Looking up another address of the bucket walks the cycle and must give up
  // after a bounded number of hops.
  EXPECT_FALSE(cache_.get(0x0A07).has_value());
  EXPECT_TRUE(cache_.get(0x0A03).has_value());
}

TEST_F(SmallCacheTest, AnOversizedStoredLengthIsClampedWhenRead) {
  cache_.push(0x0A03, {0x01, 0x02});
  Slots(cache_.data())[0].value_len = 400;  // torn length field

  const auto value = cache_.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->size(), kMaxSharedValueBytes) << "the copy must stop at the value array";

  const auto delta = cache_.get_delta(0, {0x0A03});
  ASSERT_EQ(delta.values.count(0x0A03), 1U);
  EXPECT_EQ(delta.values.at(0x0A03).size(), kMaxSharedValueBytes);
}

TEST_F(SmallCacheTest, ATornLruTailIsRebuiltBeforeEviction) {
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A02, {0x02});
  cache_.push(0x0A03, {0x03});
  cache_.push(0x0A05, {0x05});
  cache_.data()->lru_tail = -1;  // crash between unlink and re-link

  cache_.push(0x0A04, {0x04});  // must rebuild, not index at -1

  EXPECT_TRUE(cache_.get(0x0A04).has_value());
  EXPECT_EQ(cache_.size(), 4U);
}

TEST_F(SmallCacheTest, ATornLruNeighbourIsNotFollowed) {
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A02, {0x02});
  cache_.push(0x0A03, {0x03});
  cache_.push(0x0A05, {0x05});
  // The eviction victim is 0x0A01 (slot 0); tear both of its neighbours.
  Slots(cache_.data())[0].lru_prev = 4000;
  Slots(cache_.data())[0].lru_next = 4000;

  cache_.push(0x0A04, {0x04});

  EXPECT_TRUE(cache_.get(0x0A04).has_value());
  EXPECT_FALSE(cache_.get(0x0A01).has_value());
}

TEST_F(SmallCacheTest, ATornChainDuringEvictionIsNotWalkedOutOfRange) {
  cache_.push(0x0A03, {0x03});  // slot 0, bucket 3
  cache_.push(0x0A07, {0x07});  // slot 1, bucket 3, chain head
  cache_.push(0x0A01, {0x01});
  cache_.push(0x0A02, {0x02});               // table full; victim is 0x0A03 (slot 0)
  Slots(cache_.data())[1].hash_next = 4000;  // torn link in the victim's bucket

  cache_.push(0x0A06, {0x06});

  EXPECT_TRUE(cache_.get(0x0A06).has_value());
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}

TEST_F(SmallCacheTest, ACyclicChainDuringEvictionTerminates) {
  cache_.push(0x0A03, {0x03});  // slot 0, bucket 3 (the victim)
  cache_.push(0x0A07, {0x07});  // slot 1, bucket 3
  cache_.push(0x0A0B, {0x0B});  // slot 2, bucket 3, chain head
  cache_.push(0x0A01, {0x01});  // slot 3, table full
  // The chain never reaches the victim: 2 -> 1 -> 2 -> ...
  Slots(cache_.data())[2].hash_next = 1;
  Slots(cache_.data())[1].hash_next = 2;

  cache_.push(0x0A06, {0x06});  // the unlink walk must give up after capacity hops

  EXPECT_TRUE(cache_.get(0x0A06).has_value());
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}
