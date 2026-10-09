/**
 * @file tests/unit/platform/test_display_presence.cpp
 * @brief Test VDISPLAY::displayConnectorLit() against fake sysfs DRM trees.
 *
 * Whether a host has a monitor decides whether Hermes stands up a virtual
 * display at startup and for every session, and getting it wrong in either
 * direction is felt: a "yes" on a monitorless box fails the stream, a "no" on
 * a desk with a monitor swaps the user's screen for a virtual one. Each tree
 * here is the sysfs view of one of those machines.
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  /** A throwaway /sys/class/drm, removed when the test ends. */
  class FakeDrmClass: public ::testing::Test {
  protected:
    void SetUp() override {
      const auto *test = ::testing::UnitTest::GetInstance()->current_test_info();
      root = fs::temp_directory_path() /
             ("hermes-drm-class-" + std::to_string(::getpid()) + "-" + test->name());
      fs::remove_all(root);
      fs::create_directories(root);
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    void card(const std::string &name) {
      fs::create_directories(root / name);
    }

    void connector(const std::string &name, const std::string &status, const std::string &enabled) {
      const auto dir = root / name;
      fs::create_directories(dir);
      std::ofstream {dir / "status"} << status << '\n';
      std::ofstream {dir / "enabled"} << enabled << '\n';
    }

    fs::path root;
  };

}  // namespace

TEST_F(FakeDrmClass, AMonitorTheCompositorDrivesIsLit) {
  card("card1");
  connector("card1-DP-1", "disconnected", "disabled");
  connector("card1-HDMI-A-1", "connected", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {true});
}

TEST_F(FakeDrmClass, EveryConnectorUnpluggedIsDark) {
  // The issue #61 host: an iGPU and an NVIDIA card, both with their monitors
  // off the link, plus the virtual-display devices sitting idle.
  card("card1");
  connector("card1-HDMI-A-1", "disconnected", "disabled");
  connector("card1-DP-1", "disconnected", "disabled");
  card("card2");
  connector("card2-Virtual-1", "disconnected", "disabled");
  card("card3");
  connector("card3-DVI-I-1", "disconnected", "disabled");
  card("card4");
  connector("card4-DP-3", "disconnected", "disabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {false});
}

TEST_F(FakeDrmClass, AMonitorDisabledInTheDisplaySettingsIsDark) {
  card("card1");
  connector("card1-HDMI-A-1", "connected", "disabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {false});
}

TEST_F(FakeDrmClass, AConnectorThatCannotDetectButIsDrivenIsLit) {
  // Some connectors report "unknown" for good; a CRTC on one still means a
  // compositor is presenting there.
  card("card0");
  connector("card0-VGA-1", "unknown", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {true});
}

TEST_F(FakeDrmClass, AWritebackConnectorIsNotAMonitor) {
  card("card1");
  connector("card1-HDMI-A-1", "disconnected", "disabled");
  connector("card1-Writeback-1", "unknown", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {false});
}

TEST_F(FakeDrmClass, AVirtualDisplayAnotherProgramDrivesCountsAsLit) {
  // An EVDI output lit while Hermes has none up belongs to someone else - a
  // DisplayLink dock is the usual one - and the compositor can capture it.
  card("card3");
  connector("card3-DVI-I-1", "connected", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {true});
}

TEST_F(FakeDrmClass, ACardWithNoConnectorLeavesTheAnswerOpen) {
  // NVIDIA without modeset: the card exists, its monitors are lit through
  // X11, and sysfs lists no connector for it. Dark connectors elsewhere must
  // not be read as "no monitor".
  card("card0");
  connector("card0-HDMI-A-1", "disconnected", "disabled");
  card("card1");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::nullopt);
}

TEST_F(FakeDrmClass, ALitMonitorWinsOverACardSysfsCannotSee) {
  card("card0");
  connector("card0-HDMI-A-1", "connected", "enabled");
  card("card1");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {true});
}

TEST_F(FakeDrmClass, NoCardsLeavesTheAnswerOpen) {
  // A container without the host's /sys/class/drm.
  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::nullopt);
}

TEST_F(FakeDrmClass, AnUnreadableDirectoryLeavesTheAnswerOpen) {
  EXPECT_EQ(VDISPLAY::displayConnectorLit(root / "missing"), std::nullopt);
}

TEST_F(FakeDrmClass, RenderNodesAreNeitherCardsNorConnectors) {
  card("card1");
  connector("card1-HDMI-A-1", "disconnected", "disabled");
  fs::create_directories(root / "renderD128");
  std::ofstream {root / "version"} << "drm 1.1.0 20060810\n";

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {false});
}
