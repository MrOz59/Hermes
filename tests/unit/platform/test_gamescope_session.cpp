/**
 * @file tests/unit/platform/test_gamescope_session.cpp
 * @brief The Gamescope command line an isolated application session runs.
 *
 * Every case here is a session that would otherwise only be checked on a
 * machine with the Hermes-KMS driver, a private seat and a patched Gamescope.
 */
#include <gtest/gtest.h>
#include <src/platform/linux/gamescope_session.h>
#include <vector>

namespace {
  using namespace platf::gamescope;

  // Trimmed from the usage text ValveSoftware/gamescope#2220 prints.
  constexpr std::string_view prefer_drm_help =
    "  --prefer-vk-device             prefer Vulkan device for compositing (ex: 1002:7300)\n"
    "  --prefer-drm                   prefer DRM/KMS device for scanout/output, independent of the Vulkan device\n"
    "                                 also read from $GAMESCOPE_PREFER_DRM if the flag is not given\n";

  constexpr std::string_view upstream_help =
    "  --xwayland-count               create N xwayland servers\n"
    "  --prefer-vk-device             prefer Vulkan device for compositing (ex: 1002:7300)\n"
    "  -O, --prefer-output            list of connectors in order of preference\n";

  launch_t session(drm_selector_e selector, bool steam_mode) {
    return {
      .binary = "gamescope",
      .width = 1920,
      .height = 1080,
      .refresh_hz = 120,
      .connector = "Virtual-2",
      .selector = selector,
      .scanout_card = "/dev/dri/card3",
      .steam_mode = steam_mode,
      .args = {},
      .command = "steam -gamepadui",
    };
  }

  TEST(GamescopeSession, FindsPreferDrmInItsUsage) {
    EXPECT_EQ(drm_selector_from_help(prefer_drm_help), drm_selector_e::prefer_drm);
  }

  TEST(GamescopeSession, FindsDownstreamDrmDevice) {
    EXPECT_EQ(
      drm_selector_from_help("  --drm-device                   DRM device to scan out on\n"),
      drm_selector_e::drm_device
    );
  }

  TEST(GamescopeSession, UpstreamHasNoScanoutSelector) {
    EXPECT_FALSE(drm_selector_from_help(upstream_help).has_value());
    EXPECT_FALSE(drm_selector_from_help("").has_value());
  }

  TEST(GamescopeSession, PrefersTheUpstreamProposalWhenBothAreListed) {
    EXPECT_EQ(
      drm_selector_from_help("  --drm-device  x\n  --prefer-drm  y\n"),
      drm_selector_e::prefer_drm
    );
  }

  TEST(GamescopeSession, MatchesWholeOptionsOnly) {
    EXPECT_FALSE(help_lists_option("  --prefer-drm-node   other\n", "--prefer-drm"));
    EXPECT_FALSE(help_lists_option("  ---prefer-drm\n", "--prefer-drm"));
    EXPECT_TRUE(help_lists_option("--prefer-drm", "--prefer-drm"));
    EXPECT_TRUE(help_lists_option("[--prefer-drm=cardN]", "--prefer-drm"));
  }

  TEST(GamescopeSession, ReadsTheOverride) {
    EXPECT_EQ(drm_selector_from_override("--prefer-drm"), drm_selector_e::prefer_drm);
    EXPECT_EQ(drm_selector_from_override("prefer-drm"), drm_selector_e::prefer_drm);
    EXPECT_EQ(drm_selector_from_override("--drm-device"), drm_selector_e::drm_device);
    EXPECT_EQ(drm_selector_from_override("none"), drm_selector_e::none);
    EXPECT_FALSE(drm_selector_from_override("--prefer-vk-device").has_value());
    EXPECT_FALSE(drm_selector_from_override("").has_value());
  }

  TEST(GamescopeSession, AcceptsOnlyPrimaryNodes) {
    EXPECT_TRUE(is_primary_card_path("/dev/dri/card0"));
    EXPECT_TRUE(is_primary_card_path("/dev/dri/card12"));
    EXPECT_FALSE(is_primary_card_path("/dev/dri/renderD128"));
    EXPECT_FALSE(is_primary_card_path("/dev/dri/card"));
    EXPECT_FALSE(is_primary_card_path("/dev/dri/card1 -e"));
    EXPECT_FALSE(is_primary_card_path("card1"));
    EXPECT_FALSE(is_primary_card_path("/dev/dri/by-path/pci-0000:03:00.0-card"));
  }

  TEST(GamescopeSession, ScansOutOnTheSessionCard) {
    EXPECT_EQ(
      launch_command(session(drm_selector_e::prefer_drm, false)),
      "gamescope --backend=drm -W 1920 -H 1080 -w 1920 -h 1080 -r 120 --prefer-drm /dev/dri/card3 "
      "--xwayland-count 1 --expose-wayland --force-windows-fullscreen -O Virtual-2 "
      "-- steam -gamepadui"
    );
    EXPECT_EQ(
      launch_command(session(drm_selector_e::drm_device, false)),
      "gamescope --backend=drm -W 1920 -H 1080 -w 1920 -h 1080 -r 120 --drm-device /dev/dri/card3 "
      "--xwayland-count 1 --expose-wayland --force-windows-fullscreen -O Virtual-2 "
      "-- steam -gamepadui"
    );
  }

  TEST(GamescopeSession, SteamModeAddsSteamIntegrationAndASecondXwayland) {
    EXPECT_EQ(
      launch_command(session(drm_selector_e::drm_device, true)),
      "gamescope --backend=drm -W 1920 -H 1080 -w 1920 -h 1080 -r 120 --drm-device /dev/dri/card3 "
      "-e --xwayland-count 2 --expose-wayland --force-windows-fullscreen -O Virtual-2 "
      "-- steam -gamepadui"
    );
  }

  TEST(GamescopeSession, AppFlagsReplaceTheDefaultsAfterSteamMode) {
    auto launch = session(drm_selector_e::drm_device, true);
    launch.args = "--xwayland-count 1 --hdr-enabled";
    EXPECT_EQ(
      launch_command(launch),
      "gamescope --backend=drm -W 1920 -H 1080 -w 1920 -h 1080 -r 120 --drm-device /dev/dri/card3 "
      "-e --xwayland-count 2 --xwayland-count 1 --hdr-enabled -O Virtual-2 "
      "-- steam -gamepadui"
    );
  }

  TEST(GamescopeSession, NoSelectorLeavesTheDeviceToGamescope) {
    auto launch = session(drm_selector_e::none, false);
    launch.connector.clear();
    EXPECT_EQ(
      launch_command(launch),
      "gamescope --backend=drm -W 1920 -H 1080 -w 1920 -h 1080 -r 120 --xwayland-count 1 "
      "--expose-wayland --force-windows-fullscreen -- steam -gamepadui"
    );
  }

  TEST(GamescopeSession, RunsTheGivenBinary) {
    auto launch = session(drm_selector_e::prefer_drm, false);
    launch.binary = "/usr/lib/hermes-gamescope/bin/gamescope";
    EXPECT_TRUE(launch_command(launch).starts_with("/usr/lib/hermes-gamescope/bin/gamescope --backend=drm "));
    launch.binary.clear();
    EXPECT_TRUE(launch_command(launch).starts_with("gamescope --backend=drm "));
  }

  TEST(GamescopeSession, SteamModeAutoFollowsTheProgram) {
    EXPECT_TRUE(steam_mode_for("auto", "steam -gamepadui"));
    EXPECT_TRUE(steam_mode_for("auto", "  /usr/bin/steam -gamepadui"));
    EXPECT_TRUE(steam_mode_for("", "steam"));
    EXPECT_FALSE(steam_mode_for("auto", "steamtinkerlaunch run"));
    EXPECT_FALSE(steam_mode_for("auto", "env steam"));
    EXPECT_FALSE(steam_mode_for("auto", ""));
    EXPECT_TRUE(steam_mode_for("on", "heroic"));
    EXPECT_FALSE(steam_mode_for("off", "steam -gamepadui"));
  }

  TEST(GamescopeSession, TheApplicationAlwaysComesAfterTheSeparator) {
    auto launch = session(drm_selector_e::prefer_drm, true);
    launch.command = "env DXVK_HUD=1 %command% --prefer-drm /dev/dri/card0";
    const auto command = launch_command(launch);
    const auto separator = command.find(" -- ");
    ASSERT_NE(separator, std::string::npos);
    EXPECT_EQ(command.find("--prefer-drm"), command.find("--prefer-drm /dev/dri/card3"));
    EXPECT_LT(command.find("--prefer-drm /dev/dri/card3"), separator);
    EXPECT_EQ(command.substr(separator + 4), launch.command);
  }

  TEST(GamescopeSession, AppArgumentsCannotReplaceTheSessionScanoutOrNegotiatedMode) {
    for (const auto *argument : {"--backend=wayland", "--back=wayland", "--prefer-dr=/dev/dri/card0", "-eW800", "-fr30", "--prefer-drm", "--drm-device=/dev/dri/card0", "--prefer-output", "--output-width=1280", "--output-height", "--nested-width", "--nested-height", "--nested-refresh", "-W1280", "-H", "-w800", "-h", "-r144", "-OHDMI-A-1", "--"}) {
      const std::vector<std::string> arguments {argument};
      EXPECT_TRUE(session_option_override(arguments)) << argument;
    }
  }

  TEST(GamescopeSession, AppArgumentsCanChooseRenderingAndSteamPresentation) {
    const std::vector<std::string> arguments {
      "--hdr-enabled",
      "--prefer-vk-device",
      "1002:73df",
      "--xwayland-count",
      "1",
      "--expose-wayland",
      "-Ffsr",
      "-eFfsr",
      "-s0.5",
      "-C1000",
      "-e"
    };
    EXPECT_FALSE(session_option_override(arguments));
  }
}  // namespace
