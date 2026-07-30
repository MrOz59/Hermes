/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Tests for concurrent pending RTSP launch association.
 */
#include "../tests_common.h"

#include <src/rtsp.h>

namespace {
  constexpr auto accepted = rtsp_stream::launch_raise_result_e::accepted;
  constexpr auto address_conflict = rtsp_stream::launch_raise_result_e::address_conflict;
  constexpr auto unbound_pending = rtsp_stream::launch_raise_result_e::unbound_launch_pending;

  std::shared_ptr<rtsp_stream::launch_session_t> launch(
    uint32_t id,
    std::string client,
    std::string address
  ) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->id = id;
    session->unique_id = std::move(client);
    session->expected_remote_address = std::move(address);
    return session;
  }
}

TEST(PendingRtspLaunches, AssociatesDifferentClientsByAddress) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto first = launch(1, "client-a", "192.0.2.10");
  const auto second = launch(2, "client-b", "192.0.2.11");

  ASSERT_EQ(registry.insert(first, now + std::chrono::seconds {10}), accepted);
  ASSERT_EQ(registry.insert(second, now + std::chrono::seconds {10}), accepted);
  ASSERT_EQ(registry.size(), 2);
  EXPECT_EQ(registry.find_for_address("192.0.2.10"), first);
  EXPECT_EQ(registry.find_for_address("192.0.2.11"), second);
}

TEST(PendingRtspLaunches, RejectsAmbiguousConcurrentAddress) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto first = launch(1, "client-a", "198.51.100.8");
  const auto second = launch(2, "client-b", "198.51.100.8");

  ASSERT_EQ(registry.insert(first, now + std::chrono::seconds {10}), accepted);
  EXPECT_EQ(registry.insert(second, now + std::chrono::seconds {10}), address_conflict);
  EXPECT_EQ(registry.size(), 1);
  EXPECT_EQ(registry.find_for_address("198.51.100.8"), first);
}

TEST(PendingRtspLaunches, ClearsAndExpiresOnlyTheTargetLaunch) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto expired = launch(1, "client-a", "203.0.113.1");
  const auto active = launch(2, "client-b", "203.0.113.2");

  ASSERT_EQ(registry.insert(expired, now - std::chrono::seconds {1}), accepted);
  ASSERT_EQ(registry.insert(active, now + std::chrono::seconds {10}), accepted);
  const auto removed = registry.expire(now);
  ASSERT_EQ(removed.size(), 1);
  EXPECT_EQ(removed.front(), expired);
  EXPECT_EQ(registry.find_for_address("203.0.113.2"), active);

  EXPECT_EQ(registry.erase(2), active);
  EXPECT_EQ(registry.size(), 0);
}

TEST(PendingRtspLaunches, CancelsOnlyOnePairedClient) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto first = launch(1, "client-a", "192.0.2.20");
  const auto second = launch(2, "client-b", "192.0.2.21");

  ASSERT_EQ(registry.insert(first, now + std::chrono::seconds {10}), accepted);
  ASSERT_EQ(registry.insert(second, now + std::chrono::seconds {10}), accepted);
  const auto cancelled = registry.erase_client("client-a");
  ASSERT_EQ(cancelled.size(), 1);
  EXPECT_EQ(cancelled.front(), first);
  EXPECT_EQ(registry.size(), 1);
  EXPECT_EQ(registry.find_for_address("192.0.2.21"), second);
}

// A launch bound to an address must never be handed to a different peer, even
// when it is the only one pending. Otherwise a port scan (or an unreadable peer
// endpoint, which yields an empty address) would receive its encryption keys.
TEST(PendingRtspLaunches, NeverMatchesABoundLaunchToAnotherPeer) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto only = launch(1, "client-a", "192.0.2.30");

  ASSERT_EQ(registry.insert(only, now + std::chrono::seconds {10}), accepted);
  EXPECT_EQ(registry.find_for_address("192.0.2.99"), nullptr);
  EXPECT_EQ(registry.find_for_address(""), nullptr);
  EXPECT_EQ(registry.find_for_address("192.0.2.30"), only);
}

// Internal/legacy launches predate peer binding and carry no address, so they
// are matched positionally. Exactly one may be pending at a time.
TEST(PendingRtspLaunches, MatchesUnboundLegacyLaunchFromAnyPeer) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();
  const auto legacy = launch(1, "client-a", "");

  ASSERT_EQ(registry.insert(legacy, now + std::chrono::seconds {10}), accepted);
  EXPECT_EQ(registry.find_for_address("192.0.2.40"), legacy);

  // Nothing may join an unbound launch in either direction.
  EXPECT_EQ(
    registry.insert(launch(2, "client-b", ""), now + std::chrono::seconds {10}),
    unbound_pending
  );
  EXPECT_EQ(
    registry.insert(launch(3, "client-c", "192.0.2.41"), now + std::chrono::seconds {10}),
    unbound_pending
  );
  EXPECT_EQ(registry.size(), 1);
}

// Once bound launches are pending, an unbound one could steal their connection,
// so it is refused rather than queued.
TEST(PendingRtspLaunches, RefusesUnboundLaunchWhileBoundLaunchesPending) {
  rtsp_stream::pending_launch_registry_t registry;
  const auto now = rtsp_stream::pending_launch_registry_t::clock_t::now();

  ASSERT_EQ(
    registry.insert(launch(1, "client-a", "192.0.2.50"), now + std::chrono::seconds {10}),
    accepted
  );
  EXPECT_EQ(
    registry.insert(launch(2, "client-b", ""), now + std::chrono::seconds {10}),
    unbound_pending
  );
  EXPECT_EQ(registry.size(), 1);
}
