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

#include "../../mocks/mock_bus_link.h"
#include "../../src/bus/bus_connector.h"

namespace cvknx {
namespace {

// Use 1ms delays in tests to avoid waiting for real retry timers.
const int kTestDelaysMs[] = {1, 1, 1, 1, 1};

TEST(KnxdConnectorTest, FirstAttemptSucceeds) {
  MockBusLink mock;
  mock.set_connection_success(true);

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_TRUE(result);
}

TEST(KnxdConnectorTest, SecondAttemptSucceedsAfterOneFail) {
  MockBusLink mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(1);  // first call fails, second succeeds

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_TRUE(result);
}

TEST(KnxdConnectorTest, FailsAfterAllStandardRetries) {
  MockBusLink mock;
  mock.set_connection_success(true);
  // Standard: 4 attempts total (1 immediate + 3 retries)
  mock.set_connect_fail_count(10);

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, FailsAfterAllRetries) {
  MockBusLink mock;
  mock.set_connection_success(true);
  // 5 delays = 6 attempts total (1 immediate + 5 retries)
  mock.set_connect_fail_count(10);

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_FALSE(result);
  EXPECT_EQ(mock.connect_calls(), 6);
}

TEST(KnxdConnectorTest, RetriesThreeFailsThenSuccess) {
  MockBusLink mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(3);  // 3 fails, 4th attempt succeeds

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_TRUE(result);
  EXPECT_EQ(mock.connect_calls(), 4);
}

TEST(KnxdConnectorTest, FailsWhenEveryAttemptFails) {
  MockBusLink mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(4);
  const int three_delays[] = {1, 1, 1};  // 4 attempts, all of them fail

  bool result = connect_bus_with_retry(mock, three_delays);

  EXPECT_FALSE(result);
  EXPECT_EQ(mock.connect_calls(), 4);
}

TEST(KnxdConnectorTest, SucceedsOnTheLastAttempt) {
  MockBusLink mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(5);  // the 6th (and last) attempt succeeds

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_TRUE(result);
  EXPECT_EQ(mock.connect_calls(), 6);
}

TEST(KnxdConnectorTest, ConnectReturnsFalseWhenConnectionFails) {
  // When the mock's connection_success_ is false, connect() always returns false
  MockBusLink mock;
  mock.set_connection_success(false);

  bool result = connect_bus_with_retry(mock, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, DefaultDelaysSucceedOnTheFirstAttempt) {
  // Exercises the built-in delay table (empty span): a successful first
  // attempt never sleeps.
  MockBusLink mock;
  mock.set_connection_success(true);
  EXPECT_TRUE(connect_bus_with_retry(mock));
}

TEST(KnxdConnectorTest, AttemptCountFollowsTheDelaySpan) {
  // One delay means two attempts — a short span can never read past its end.
  MockBusLink mock;
  mock.set_connection_success(false);
  const int one_delay[] = {1};
  EXPECT_FALSE(connect_bus_with_retry(mock, one_delay));
  EXPECT_EQ(mock.connect_calls(), 2);
}

TEST(KnxdConnectorTest, AZeroDelayRetriesImmediately) {
  MockBusLink mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(1);
  const int no_delay[] = {0};

  EXPECT_TRUE(connect_bus_with_retry(mock, no_delay));
  EXPECT_EQ(mock.connect_calls(), 2);
}

}  // namespace
}  // namespace cvknx
