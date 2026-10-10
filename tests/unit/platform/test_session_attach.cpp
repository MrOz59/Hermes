/**
 * @file tests/unit/platform/test_session_attach.cpp
 * @brief Test when a Hermes started before login decides to join the session.
 *
 * The decision is made from two environments: the one Hermes was started with
 * and the one the systemd user manager holds. Getting it wrong either restarts
 * a Hermes that was fine, or leaves one that cannot reach the desktop.
 */
#include "../../tests_common.h"

#include <src/platform/linux/session_attach.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using platf::session_attach::attach_needed;
using platf::session_attach::display_server_reachable;
using platf::session_attach::environment_t;
using platf::session_attach::merged_environment;
using platf::session_attach::parse_environment;
using platf::session_attach::parse_environment_block;

namespace {

  /** What a lingering user manager hands a unit at boot, before any login. */
  const environment_t before_login {
    {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"},
    {"XDG_RUNTIME_DIR", "/run/user/1000"},
    {"HOME", "/home/user"},
  };

  /** What Plasma publishes into the user manager at login. */
  const environment_t plasma {
    {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"},
    {"XDG_RUNTIME_DIR", "/run/user/1000"},
    {"HOME", "/home/user"},
    {"WAYLAND_DISPLAY", "wayland-0"},
    {"DISPLAY", ":0"},
    {"XDG_CURRENT_DESKTOP", "KDE"},
    {"XDG_SESSION_TYPE", "wayland"},
  };

}  // namespace

TEST(SessionAttach, AHermesStartedBeforeLoginJoinsTheSession) {
  EXPECT_TRUE(attach_needed(before_login, plasma));
}

TEST(SessionAttach, AHermesStartedInsideTheSessionIsLeftAlone) {
  EXPECT_FALSE(attach_needed(plasma, plasma));
}

TEST(SessionAttach, NobodyLoggedInIsNothingToJoin) {
  EXPECT_FALSE(attach_needed(before_login, before_login));
}

TEST(SessionAttach, ASessionThatPublishesNoDisplayServerIsNothingToJoin) {
  // gamescope-session: a desktop name, and no display variables at all.
  auto game_mode = before_login;
  game_mode["XDG_CURRENT_DESKTOP"] = "gamescope";

  EXPECT_FALSE(attach_needed(before_login, game_mode));
}

TEST(SessionAttach, LoggingInToAnotherDesktopIsADifferentSession) {
  auto hyprland = plasma;
  hyprland["WAYLAND_DISPLAY"] = "wayland-1";
  hyprland["XDG_CURRENT_DESKTOP"] = "Hyprland";

  EXPECT_TRUE(attach_needed(plasma, hyprland));
}

TEST(SessionAttach, VariablesThatSelectNoDisplayServerDoNotCount) {
  auto session = plasma;
  session["KDE_SESSION_VERSION"] = "6";
  session["PATH"] = "/usr/local/bin:/usr/bin";

  EXPECT_FALSE(attach_needed(plasma, session));
}

TEST(SessionAttach, AVariableTheSessionDroppedIsNotAReasonToRestart) {
  // An X11 session after a Wayland one: Hermes still carries WAYLAND_DISPLAY,
  // and DISPLAY changing is what says so.
  auto own = plasma;
  auto x11 = plasma;
  x11.erase("WAYLAND_DISPLAY");
  EXPECT_FALSE(attach_needed(own, x11));

  x11["XDG_SESSION_TYPE"] = "x11";
  EXPECT_TRUE(attach_needed(own, x11));
}

TEST(SessionAttach, TheRestartKeepsWhatHermesHadAndTakesWhatTheSessionSays) {
  auto own = before_login;
  own["HERMES_ONLY"] = "kept";
  own["HOME"] = "/home/user";

  const auto merged = parse_environment(merged_environment(own, plasma));

  EXPECT_EQ(merged.at("HERMES_ONLY"), "kept");
  EXPECT_EQ(merged.at("WAYLAND_DISPLAY"), "wayland-0");
  EXPECT_EQ(merged.at("XDG_CURRENT_DESKTOP"), "KDE");
  EXPECT_FALSE(attach_needed(merged, plasma));
}

TEST(SessionAttach, AssignmentsSplitOnTheFirstEqualsSign) {
  const auto parsed = parse_environment({"A=1", "B=x=y", "EMPTY=", "=nameless", "no-equals"});

  ASSERT_EQ(parsed.size(), 3u);
  EXPECT_EQ(parsed.at("A"), "1");
  EXPECT_EQ(parsed.at("B"), "x=y");
  EXPECT_EQ(parsed.at("EMPTY"), "");
}

TEST(SessionAttach, TheKernelsCopyOfTheEnvironmentIsNulSeparated) {
  using namespace std::string_view_literals;
  const auto parsed = parse_environment_block("HOME=/home/user\0XDG_RUNTIME_DIR=/run/user/1000\0EMPTY=\0"sv);

  ASSERT_EQ(parsed.size(), 3u);
  EXPECT_EQ(parsed.at("HOME"), "/home/user");
  EXPECT_EQ(parsed.at("XDG_RUNTIME_DIR"), "/run/user/1000");
  EXPECT_EQ(parsed.at("EMPTY"), "");
}

TEST(SessionAttach, WhatHermesSetForItselfDoesNotReachTheRestart) {
  // Before login the tray forces QT_QPA_PLATFORM=minimal on its own process.
  // A restart built from the live environment would hand that to every Qt
  // tool Hermes runs afterwards.
  ASSERT_EQ(::setenv("HERMES_TEST_SET_AFTER_START", "minimal", 1), 0);

  const auto initial = platf::session_attach::initial_environment();

  EXPECT_EQ(initial.count("HERMES_TEST_SET_AFTER_START"), 0u);
  EXPECT_FALSE(initial.empty());
  ::unsetenv("HERMES_TEST_SET_AFTER_START");
}

namespace {

  namespace fs = std::filesystem;

  /** A runtime directory with, optionally, a compositor listening in it. */
  class FakeRuntimeDir: public ::testing::Test {
  protected:
    void SetUp() override {
      const auto *test = ::testing::UnitTest::GetInstance()->current_test_info();
      root = fs::temp_directory_path() / ("hermes-rt-" + std::to_string(::getpid()) + "-" + test->name());
      fs::remove_all(root);
      fs::create_directories(root);
    }

    void TearDown() override {
      if (listener >= 0) {
        ::close(listener);
      }
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    /** Bind @p name under the directory; a compositor also listens on it. */
    void socket_at(const std::string &name, bool listening) {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto path = (root / name).string();
      ASSERT_LT(path.size(), sizeof(address.sun_path));
      path.copy(address.sun_path, path.size());
      listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
      ASSERT_GE(listener, 0);
      ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
      if (listening) {
        ASSERT_EQ(::listen(listener, 1), 0);
      }
    }

    environment_t wayland(const std::string &name) const {
      return {{"WAYLAND_DISPLAY", name}, {"XDG_RUNTIME_DIR", root.string()}};
    }

    fs::path root;
    int listener {-1};
  };

  /** Set or clear a variable for one test and put it back afterwards. */
  class ScopedEnv {
  public:
    ScopedEnv(std::string name, std::optional<std::string> value):
        name {std::move(name)} {
      if (const char *old = std::getenv(this->name.c_str())) {
        previous = old;
      }
      apply(value);
    }

    ~ScopedEnv() {
      apply(previous);
    }

  private:
    void apply(const std::optional<std::string> &value) const {
      if (value) {
        ::setenv(name.c_str(), value->c_str(), 1);
      } else {
        ::unsetenv(name.c_str());
      }
    }

    std::string name;
    std::optional<std::string> previous;
  };

}  // namespace

TEST_F(FakeRuntimeDir, ACompositorListeningOnItsSocketIsReachable) {
  socket_at("wayland-0", true);

  EXPECT_TRUE(display_server_reachable(wayland("wayland-0")));
}

TEST_F(FakeRuntimeDir, ASocketPathCanBeGivenWhole) {
  socket_at("wayland-0", true);

  EXPECT_TRUE(display_server_reachable({{"WAYLAND_DISPLAY", (root / "wayland-0").string()}}));
}

TEST_F(FakeRuntimeDir, ANameWithNoSocketBehindItIsNotReachable) {
  EXPECT_FALSE(display_server_reachable(wayland("wayland-0")));
}

TEST_F(FakeRuntimeDir, ASocketFileNobodyListensOnIsNotReachable) {
  // What a compositor that died without cleaning up leaves in the directory.
  socket_at("wayland-0", false);

  EXPECT_FALSE(display_server_reachable(wayland("wayland-0")));
}

TEST_F(FakeRuntimeDir, NoDisplayVariablesIsNotReachable) {
  EXPECT_FALSE(display_server_reachable({{"XDG_RUNTIME_DIR", root.string()}}));
}

TEST(DisplayServer, AnXDisplayOnAnotherHostIsNotSecondGuessed) {
  EXPECT_TRUE(display_server_reachable({{"DISPLAY", "otherhost:10.0"}}));
}

TEST(DisplayServer, ALocalXDisplayThatDoesNotExistIsNotReachable) {
  EXPECT_FALSE(display_server_reachable({{"DISPLAY", ":4094"}}));
  EXPECT_FALSE(display_server_reachable({{"DISPLAY", "garbage"}}));
  EXPECT_FALSE(display_server_reachable({{"DISPLAY", ":not-a-number"}}));
}

TEST_F(FakeRuntimeDir, VariablesOfASessionThatEndedAreForgotten) {
  const ScopedEnv runtime {"XDG_RUNTIME_DIR", root.string()};
  const ScopedEnv wayland_display {"WAYLAND_DISPLAY", "wayland-0"};
  const ScopedEnv display {"DISPLAY", ":4094"};
  const ScopedEnv desktop {"XDG_CURRENT_DESKTOP", "KDE"};

  platf::session_attach::drop_stale_display_environment();

  EXPECT_EQ(std::getenv("WAYLAND_DISPLAY"), nullptr);
  EXPECT_EQ(std::getenv("DISPLAY"), nullptr);
  EXPECT_EQ(std::getenv("XDG_CURRENT_DESKTOP"), nullptr);
}

TEST_F(FakeRuntimeDir, VariablesOfARunningSessionAreKept) {
  socket_at("wayland-0", true);
  const ScopedEnv runtime {"XDG_RUNTIME_DIR", root.string()};
  const ScopedEnv wayland_display {"WAYLAND_DISPLAY", "wayland-0"};
  const ScopedEnv display {"DISPLAY", std::nullopt};
  const ScopedEnv desktop {"XDG_CURRENT_DESKTOP", "KDE"};

  platf::session_attach::drop_stale_display_environment();

  ASSERT_NE(std::getenv("WAYLAND_DISPLAY"), nullptr);
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-0");
  EXPECT_STREQ(std::getenv("XDG_CURRENT_DESKTOP"), "KDE");
}
