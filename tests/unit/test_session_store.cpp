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
 * @file test_session_store.cpp
 * @brief Session store: lifetime, eviction and the shared-memory contract.
 *
 * The store lives in shared memory because a session created by one worker
 * must be valid in every other worker (a client logs in once and then sends
 * `s=<id>` with requests that any worker may serve).
 */

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "state/session_store.h"

using namespace cvknx;

TEST(SessionStoreTest, CreateAnonymousSession) {
  SessionStore store;
  const std::string id = store.create_session(true);
  EXPECT_EQ(id, std::string{kAnonymousSessionId});
  EXPECT_EQ(store.count(), 0U);  // anonymous sessions never occupy a slot
}

TEST(SessionStoreTest, CreateNormalSession) {
  SessionStore store;
  const std::string id = store.create_session(false);
  EXPECT_NE(id, std::string{kAnonymousSessionId});
  EXPECT_EQ(id.size(), kSessionIdChars);
  EXPECT_EQ(store.count(), 1U);
}

TEST(SessionStoreTest, ValidateSession) {
  SessionStore store;
  const std::string id = store.create_session(false);
  EXPECT_TRUE(store.is_valid(id));
}

TEST(SessionStoreTest, AnonymousAlwaysValid) {
  SessionStore store;
  EXPECT_TRUE(store.is_valid(kAnonymousSessionId));
}

TEST(SessionStoreTest, InvalidSession) {
  SessionStore store;
  EXPECT_FALSE(store.is_valid("nonexistent"));
  EXPECT_FALSE(store.is_valid(""));
}

TEST(SessionStoreTest, RemoveSession) {
  SessionStore store;
  const std::string id = store.create_session(false);
  EXPECT_TRUE(store.is_valid(id));
  store.remove(id);
  EXPECT_FALSE(store.is_valid(id));
  EXPECT_EQ(store.count(), 0U);
}

TEST(SessionStoreTest, RemoveAnonymousAndUnknownAreNoOps) {
  SessionStore store;
  const std::string id = store.create_session(false);
  store.remove(kAnonymousSessionId);
  store.remove("does-not-exist");
  EXPECT_TRUE(store.is_valid(kAnonymousSessionId));
  EXPECT_TRUE(store.is_valid(id));
  EXPECT_EQ(store.count(), 1U);
}

TEST(SessionStoreTest, MultipleSessionsAreDistinct) {
  SessionStore store;
  const auto id1 = store.create_session(false);
  const auto id2 = store.create_session(false);
  EXPECT_NE(id1, id2);
  EXPECT_TRUE(store.is_valid(id1));
  EXPECT_TRUE(store.is_valid(id2));
  EXPECT_EQ(store.count(), 2U);
}

TEST(SessionStoreTest, ExpiredSessionsCleanedOnIsValid) {
  SessionStore store(/*session_ttl_sec=*/0);  // sessions expire immediately
  const std::string id = store.create_session(false);
  EXPECT_EQ(store.count(), 1U);

  EXPECT_FALSE(store.is_valid(id));
  EXPECT_EQ(store.count(), 0U);
}

TEST(SessionStoreTest, CleanupRemovesExpiredSessions) {
  SessionStore store(/*session_ttl_sec=*/0);
  const std::string id = store.create_session();
  EXPECT_EQ(store.count(), 1U);
  store.cleanup_expired();
  EXPECT_EQ(store.count(), 0U);
  EXPECT_FALSE(store.is_valid(id));
}

// ===========================================================================
// Capacity: the oldest session is evicted, the store never refuses a login
// ===========================================================================

TEST(SessionStoreTest, EvictsTheOldestSessionWhenFull) {
  SessionStore store(1800, 2);  // 30-min TTL, max 2 sessions
  EXPECT_EQ(store.capacity(), 2U);

  const auto id1 = store.create_session(false);
  const auto id2 = store.create_session(false);
  ASSERT_EQ(store.count(), 2U);

  const auto id3 = store.create_session(false);
  EXPECT_EQ(store.count(), 2U);
  EXPECT_EQ(store.evictions(), 1U);
  EXPECT_TRUE(store.is_valid(id3));
  EXPECT_TRUE(store.is_valid(id2));   // newer session preserved
  EXPECT_FALSE(store.is_valid(id1));  // oldest evicted
}

TEST(SessionStoreTest, ExceedingTheCapacityManyTimesKeepsOnlyTheNewest) {
  constexpr std::size_t kCapacity = 8;
  SessionStore store(1800, kCapacity);

  std::vector<std::string> ids;
  for (std::size_t i = 0; i < 50; ++i) {
    ids.push_back(store.create_session(false));
    EXPECT_EQ(store.count(), std::min(i + 1, kCapacity));
  }
  EXPECT_EQ(store.count(), kCapacity);
  EXPECT_EQ(store.evictions(), 50 - kCapacity);

  // The last `capacity` sessions survive, every older one is gone.
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const bool should_live = i >= ids.size() - kCapacity;
    EXPECT_EQ(store.is_valid(ids[i]), should_live) << "session " << i;
  }
}

TEST(SessionStoreTest, AnonymousSessionDoesNotConsumeCapacity) {
  SessionStore store(1800, 1);
  const auto id1 = store.create_session(false);
  EXPECT_EQ(store.count(), 1U);

  const auto anon = store.create_session(true);
  EXPECT_EQ(anon, std::string{kAnonymousSessionId});
  EXPECT_EQ(store.count(), 1U);
  EXPECT_TRUE(store.is_valid(kAnonymousSessionId));
  EXPECT_TRUE(store.is_valid(id1));
}

TEST(SessionStoreTest, RemovedSlotsAreReused) {
  SessionStore store(1800, 2);
  const auto id1 = store.create_session(false);
  const auto id2 = store.create_session(false);
  store.remove(id1);
  EXPECT_EQ(store.count(), 1U);

  const auto id3 = store.create_session(false);
  EXPECT_EQ(store.count(), 2U);
  EXPECT_EQ(store.evictions(), 0U) << "a free slot must be reused, not evicted";
  EXPECT_TRUE(store.is_valid(id2));
  EXPECT_TRUE(store.is_valid(id3));
}

// ===========================================================================
// Shared memory: the same session is visible through another instance
// ===========================================================================

TEST(SessionStoreTest, TwoInstancesShareTheSameRegion) {
  SessionStore owner;
  SessionStore worker;
  worker.attach(owner.data());

  const auto id = worker.create_session(false);
  EXPECT_TRUE(owner.is_valid(id));
  EXPECT_EQ(owner.count(), 1U);

  owner.remove(id);
  EXPECT_FALSE(worker.is_valid(id));
}

/// Two *processes* (a fork, as the worker pool does it) must see the same
/// sessions: this is the regression test for "sessions are per worker".
TEST(SessionStoreTest, SessionsAreVisibleInAForkedChild) {
  SessionStore store;
  const std::string id = store.create_session(false);

  const pid_t pid = ::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Child: validate the session created by the parent and create one itself.
    const bool parent_visible = store.is_valid(id);
    const std::string child_id = store.create_session(false);
    ::_exit(parent_visible && !child_id.empty() && store.is_valid(child_id) ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "a child process did not see the parent's session";
  // The child's session is visible in the parent as well.
  EXPECT_EQ(store.count(), 2U);
}

TEST(SessionStoreTest, UninitialisedStoreToleratesEveryCall) {
  SessionStore store;
  store.attach(nullptr);
  EXPECT_FALSE(store.is_initialized());
  EXPECT_FALSE(store.is_valid("abc"));
  EXPECT_TRUE(store.is_valid(kAnonymousSessionId));
  store.remove("abc");
  store.cleanup_expired();
  EXPECT_EQ(store.count(), 0U);
  EXPECT_EQ(store.capacity(), 0U);
  EXPECT_EQ(store.evictions(), 0U);
}

TEST(SessionStoreTest, RegionSizeScalesWithCapacity) {
  EXPECT_GT(session_region_size(64), session_region_size(8));
  EXPECT_EQ(SessionStore::region_size(4), session_region_size(4));
  // The capacity is clamped into the supported range.
  SessionStore clamped(1800, 0);
  EXPECT_EQ(clamped.capacity(), kMinMaxSessions);
}

/// A process that dies while holding the shared mutex leaves it inconsistent;
/// the next locker gets EOWNERDEAD and must recover.
TEST(SessionStoreRecovery, OwnerDeathIsRecoveredByEveryAccessor) {
  SessionStore store;
  const auto kill_owner = [&store]() {
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      ::pthread_mutex_lock(&store.data()->mutex);
      ::_Exit(0);  // never unlock, never flush coverage data
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  };

  kill_owner();
  const std::string id = store.create_session(false);

  kill_owner();
  EXPECT_TRUE(store.is_valid(id));

  kill_owner();
  EXPECT_EQ(store.count(), 1U);

  kill_owner();
  store.cleanup_expired();
  store.remove(id);

  kill_owner();
  EXPECT_FALSE(store.is_valid(id));
  EXPECT_EQ(store.evictions(), 0U);
}

TEST(SessionStoreRecovery, CreateOnAnUnattachedStoreReturnsNoSession) {
  SessionStore store;
  store.attach(nullptr);
  EXPECT_TRUE(store.create_session(false).empty());
}

/// A wall clock that steps backwards must not invalidate every session: the
/// unsigned difference would wrap to ~136 years of age.
TEST(SessionStoreRecovery, AClockStepBackwardDoesNotExpireSessions) {
  SessionStore store;
  const std::string id = store.create_session(false);
  ASSERT_FALSE(id.empty());

  // Move the creation timestamp into the future (the clock was stepped back).
  auto* entries = reinterpret_cast<SharedSessionEntry*>(store.data() + 1);
  for (std::uint32_t i = 0; i < store.data()->capacity; ++i) {
    if (entries[i].id[0] != '\0') {
      entries[i].created += 3600;
    }
  }

  EXPECT_TRUE(store.is_valid(id));
}
