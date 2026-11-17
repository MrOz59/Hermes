/**
 * @file tests/unit/platform/test_window_placement.cpp
 * @brief Placement must recognize supported command quoting and Steam entry types.
 */
#ifdef __linux__
#include <gtest/gtest.h>
#include <boost/program_options/parsers.hpp>
#include <src/platform/linux/window_placement.h>

namespace {
  using boost::program_options::split_unix;
  using platf::window_placement::launches_gamescope;
  using platf::window_placement::opens_big_picture;
}

TEST(WindowPlacement, RecognizesQuotedNativeAndFlatpakBigPictureCommands) {
  for (const auto *command : {
         "setsid steam steam://open/bigpicture",
         "\"/usr/bin/steam\" 'steam://open/bigpicture'",
         "'/path with spaces/steam' -gamepadui",
         "env FOO=bar /usr/bin/steam -bigpicture",
         "steam -tenfoot",
         "steam 'steam://open/bigpicture?foo=bar'",
         "flatpak run com.valvesoftware.Steam steam://open/bigpicture",
         "\"/usr/bin/flatpak\" run --branch=stable com.valvesoftware.Steam -gamepadui",
       }) {
    EXPECT_TRUE(opens_big_picture(split_unix(command))) << command;
  }
}

TEST(WindowPlacement, DirectGameLaunchesDoNotMoveTheSteamClient) {
  for (const auto *command : {
         "steam steam://rungameid/588650",
         "steam -applaunch 588650",
         "flatpak run com.valvesoftware.Steam steam://rungameid/588650",
         "steam steam://open/bigpicture-unrelated",
         "konsole -gamepadui",
         "",
       }) {
    EXPECT_FALSE(opens_big_picture(split_unix(command))) << command;
  }
}

TEST(WindowPlacement, RecognizesQuotedGamescopeAndTheShippedLauncher) {
  EXPECT_TRUE(launches_gamescope(split_unix("\"/usr/bin/gamescope\" -f -- game")));
  EXPECT_TRUE(launches_gamescope(split_unix("'/path with spaces/hermes-gamescope-launch'")));
  EXPECT_FALSE(launches_gamescope(split_unix("steam -bigpicture")));
}
#endif
