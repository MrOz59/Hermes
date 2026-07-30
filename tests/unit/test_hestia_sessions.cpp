/**
 * @file tests/unit/test_hestia_sessions.cpp
 * @brief Tests for Hestia API reservation ownership and lifecycle tokens.
 */
#include "../tests_common.h"

#include <src/nvhttp.h>

TEST(HestiaSessionReservations, StopTokenCannotClearAnotherReservation) {
  constexpr auto client = "client-a";
  nvhttp::store_hestia_session_prepare(client, {
    .session_id = "hestia-session-a",
    .virtual_display = true,
    .isolated = true,
    .width = 1920,
    .height = 1080,
    .fps = 60,
  });

  EXPECT_FALSE(
    nvhttp::clear_hestia_session_prepare(client, "hestia-session-b")
  );

  const auto prepare = nvhttp::take_hestia_session_prepare(client);
  ASSERT_TRUE(prepare.has_value());
  EXPECT_EQ(prepare->session_id, "hestia-session-a");
  EXPECT_TRUE(prepare->isolated);
}

TEST(HestiaSessionReservations, ReservationsAreIsolatedByPairedClient) {
  nvhttp::store_hestia_session_prepare("client-a", {
    .session_id = "hestia-session-a",
  });
  nvhttp::store_hestia_session_prepare("client-b", {
    .session_id = "hestia-session-b",
  });

  EXPECT_TRUE(
    nvhttp::clear_hestia_session_prepare("client-a", "hestia-session-a")
  );
  const auto second = nvhttp::take_hestia_session_prepare("client-b");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->session_id, "hestia-session-b");
}
