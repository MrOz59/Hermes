/**
 * @file tests/unit/platform/test_session_bus.cpp
 * @brief Test the session bus given to isolated sessions of the Hermes user.
 */
#include "../../tests_common.h"

#include <src/platform/linux/session_bus.h>

using platf::session_bus::with_private_bus;

TEST(SessionBus, ASessionWithoutABusGetsAPrivateOne) {
  // Haven, 2026-11-24: Steam in an isolated Gamescope session exited with
  // "Can't find session bus" and took the session with it.
  EXPECT_EQ(with_private_bus("/usr/lib/hermes-gamescope/bin/gamescope --backend=drm -- hermes-gamescope-launch", false, "/usr/bin/dbus-run-session"),
            "/usr/bin/dbus-run-session -- /usr/lib/hermes-gamescope/bin/gamescope --backend=drm -- hermes-gamescope-launch");
}

TEST(SessionBus, ASessionThatHasABusKeepsIt) {
  EXPECT_EQ(with_private_bus("gamescope -- game", true, "/usr/bin/dbus-run-session"), "gamescope -- game");
}

TEST(SessionBus, NoBusCanBeStartedWithoutDbusRunSession) {
  EXPECT_EQ(with_private_bus("gamescope -- game", false, ""), std::nullopt);
}

TEST(SessionBus, APathThatWouldNotSurviveTheCommandLineIsRefused) {
  EXPECT_EQ(with_private_bus("gamescope -- game", false, "/opt/my tools/dbus-run-session"), std::nullopt);
  EXPECT_EQ(with_private_bus("gamescope -- game", false, "/usr/bin/dbus'run"), std::nullopt);
}
