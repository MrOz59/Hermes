/**
 * @file tests/unit/platform/test_common.cpp
 * @brief Test src/platform/common.*.
 */
#include "../../tests_common.h"

#include <boost/asio/ip/host_name.hpp>
#include <src/platform/common.h>

struct SetEnvTest: ::testing::TestWithParam<std::tuple<std::string, std::string, int>> {
protected:
  void TearDown() override {
    // Clean up environment variable after each test
    const auto &[name, value, expected] = GetParam();
    platf::unset_env(name);
  }
};

TEST_P(SetEnvTest, SetEnvironmentVariableTests) {
  const auto &[name, value, expected] = GetParam();
  platf::set_env(name, value);

  const char *env_value = std::getenv(name.c_str());
  if (expected == 0 && !value.empty()) {
    ASSERT_NE(env_value, nullptr);
    ASSERT_EQ(std::string(env_value), value);
  } else {
    ASSERT_EQ(env_value, nullptr);
  }
}

TEST_P(SetEnvTest, UnsetEnvironmentVariableTests) {
  const auto &[name, value, expected] = GetParam();
  platf::unset_env(name);

  const char *env_value = std::getenv(name.c_str());
  if (expected == 0) {
    ASSERT_EQ(env_value, nullptr);
  }
}

INSTANTIATE_TEST_SUITE_P(
  SetEnvTests,
  SetEnvTest,
  ::testing::Values(
    std::make_tuple("SUNSHINE_UNIT_TEST_ENV_VAR", "test_value_0", 0),
    std::make_tuple("SUNSHINE_UNIT_TEST_ENV_VAR", "test_value_1", 0),
    std::make_tuple("", "test_value", -1)
  )
);

TEST(HostnameTests, TestAsioEquality) {
  // These should be equivalent on all platforms for ASCII hostnames
  ASSERT_EQ(platf::get_host_name(), boost::asio::ip::host_name());
}

#ifdef __linux__
// Detection of a standalone Gamescope session (SteamOS Game Mode) is driven by
// XDG_CURRENT_DESKTOP / GAMESCOPE_WAYLAND_DISPLAY and takes precedence over the
// desktop window-system, so it is testable without a real compositor.
struct SessionEnvironmentTest: ::testing::Test {
protected:
  void TearDown() override {
    platf::unset_env("XDG_CURRENT_DESKTOP");
    platf::unset_env("GAMESCOPE_WAYLAND_DISPLAY");
  }
};

TEST_F(SessionEnvironmentTest, GamescopeSessionFromDesktopName) {
  platf::set_env("XDG_CURRENT_DESKTOP", "gamescope");
  const auto env = platf::detect_session_environment();
  ASSERT_EQ(env.kind, platf::session_environment_e::gamescope_session);
  ASSERT_EQ(env.describe(), "gamescope_session");
  ASSERT_TRUE(env.gamescope_running);
  ASSERT_FALSE(env.can_host_virtual_display);
}

TEST_F(SessionEnvironmentTest, GamescopeSessionDesktopNameIsCaseInsensitive) {
  platf::set_env("XDG_CURRENT_DESKTOP", "Gamescope");
  ASSERT_EQ(platf::detect_session_environment().kind, platf::session_environment_e::gamescope_session);
}

TEST_F(SessionEnvironmentTest, GamescopeRunningFlagFromWaylandDisplay) {
  // A Gamescope socket present without XDG_CURRENT_DESKTOP=gamescope means a
  // nested Gamescope is available, but the session is not the Gamescope session.
  platf::set_env("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0");
  const auto env = platf::detect_session_environment();
  ASSERT_TRUE(env.gamescope_running);
  ASSERT_NE(env.kind, platf::session_environment_e::gamescope_session);
}

TEST_F(SessionEnvironmentTest, NonGamescopeDesktopIsNotGamescopeSession) {
  platf::set_env("XDG_CURRENT_DESKTOP", "KDE");
  const auto env = platf::detect_session_environment();
  ASSERT_NE(env.kind, platf::session_environment_e::gamescope_session);
}
#endif
