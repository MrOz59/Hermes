/**
 * @file tests/unit/platform/test_display_presence.cpp
 * @brief Test VDISPLAY::displayConnectorLit(), waitForConnectorEnabled() and removalStrandsGreeter()
 *        against fake sysfs DRM trees.
 *
 * Whether a host has a monitor decides whether Hermes stands up a virtual
 * display at startup and for every session, and getting it wrong in either
 * direction is felt: a "yes" on a monitorless box fails the stream, a "no" on
 * a desk with a monitor swaps the user's screen for a virtual one. Each tree
 * here is the sysfs view of one of those machines.
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

// Before login nothing can configure the virtual output, so a session waits for
// the greeter to light it by itself. These trees are what that wait reads.

TEST_F(FakeDrmClass, AConnectorTheGreeterAlreadyLitNeedsNoWait) {
  card("card0");
  connector("card0-Virtual-1", "connected", "enabled");

  EXPECT_TRUE(VDISPLAY::waitForConnectorEnabled("Virtual-1", std::chrono::milliseconds {0}, root));
}

TEST_F(FakeDrmClass, AConnectorNothingLitsTimesOut) {
  card("card0");
  // The X11 greeter's view: hotplugged and probed, never bound to a CRTC.
  connector("card0-Virtual-1", "connected", "disabled");

  EXPECT_FALSE(VDISPLAY::waitForConnectorEnabled("Virtual-1", std::chrono::milliseconds {250}, root));
}

TEST_F(FakeDrmClass, OnlyTheNamedConnectorCounts) {
  card("card0");
  card("card6");
  connector("card0-Virtual-1", "connected", "disabled");
  connector("card6-Virtual-11", "connected", "enabled");

  EXPECT_FALSE(VDISPLAY::waitForConnectorEnabled("Virtual-1", std::chrono::milliseconds {0}, root));
}

TEST_F(FakeDrmClass, TheWaitEndsWhenTheGreeterLightsTheConnector) {
  card("card0");
  connector("card0-Virtual-1", "disconnected", "disabled");
  std::thread greeter {[this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds {300});
    connector("card0-Virtual-1", "connected", "enabled");
  }};

  EXPECT_TRUE(VDISPLAY::waitForConnectorEnabled("Virtual-1", std::chrono::seconds {5}, root));
  greeter.join();
}

TEST_F(FakeDrmClass, AnEmptyConnectorNameNeverMatches) {
  card("card0");
  connector("card0-Virtual-1", "connected", "enabled");

  EXPECT_FALSE(VDISPLAY::waitForConnectorEnabled("", std::chrono::milliseconds {0}, root));
}

// A display removed while the login greeter is on the seat keeps its output
// connected when that output is all the greeter has. These are the cases the
// decision turns on.

TEST_F(FakeDrmClass, TheOutputKeptForTheGreeterIsNotAMonitor) {
  card("card0");
  card("card1");
  connector("card0-HDMI-A-1", "disconnected", "disabled");
  connector("card1-Virtual-1", "connected", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root), std::optional<bool> {true});
  EXPECT_EQ(VDISPLAY::displayConnectorLit(root, "Virtual-1"), std::optional<bool> {false});
}

TEST_F(FakeDrmClass, AMonitorNextToTheIgnoredOutputStillCounts) {
  card("card0");
  card("card1");
  connector("card0-HDMI-A-1", "connected", "enabled");
  connector("card1-Virtual-1", "connected", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root, "Virtual-1"), std::optional<bool> {true});
}

TEST_F(FakeDrmClass, IgnoringOneConnectorDoesNotIgnoreItsNamesake) {
  card("card1");
  card("card6");
  connector("card1-Virtual-1", "connected", "disabled");
  connector("card6-Virtual-11", "connected", "enabled");

  EXPECT_EQ(VDISPLAY::displayConnectorLit(root, "Virtual-1"), std::optional<bool> {true});
}

TEST(GreeterOutput, TheGreetersOnlyOutputIsKept) {
  EXPECT_TRUE(VDISPLAY::removalStrandsGreeter(true, true, std::optional<bool> {false}));
}

TEST(GreeterOutput, WhenSysfsCannotSayTheOutputIsKept) {
  // A spare screen at the login prompt is the cheaper mistake.
  EXPECT_TRUE(VDISPLAY::removalStrandsGreeter(true, true, std::nullopt));
}

TEST(GreeterOutput, AGreeterWithARealMonitorLosesNothing) {
  EXPECT_FALSE(VDISPLAY::removalStrandsGreeter(true, true, std::optional<bool> {true}));
}

TEST(GreeterOutput, ADesktopSessionSurvivesLosingTheOutput) {
  EXPECT_FALSE(VDISPLAY::removalStrandsGreeter(false, true, std::optional<bool> {false}));
}

TEST(GreeterOutput, AnOutputNothingDrivesIsNotWhatTheGreeterIsOn) {
  // The X11 greeter never lights a virtual output; keeping it would hold a
  // connector for nobody.
  EXPECT_FALSE(VDISPLAY::removalStrandsGreeter(true, false, std::optional<bool> {false}));
}

// The desktop's tools are called with a deadline: kscreen-doctor has been seen
// never to answer when asked while Plasma was still starting.

TEST(BoundedCommand, OutputOfACommandThatSucceedsIsReturned) {
  bool timed_out = true;
  const auto output = VDISPLAY::boundedCommandOutput("printf 'one\\ntwo\\n'", std::chrono::seconds {5}, &timed_out);

  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(*output, "one\ntwo\n");
  EXPECT_FALSE(timed_out);
}

TEST(BoundedCommand, ACommandThatFailsHasNoOutput) {
  bool timed_out = true;
  EXPECT_FALSE(VDISPLAY::boundedCommandOutput("echo partial; exit 3", std::chrono::seconds {5}, &timed_out).has_value());
  EXPECT_FALSE(timed_out);
}

TEST(BoundedCommand, ACommandThatNeverAnswersIsStopped) {
  bool timed_out = false;
  const auto started = std::chrono::steady_clock::now();
  const auto output = VDISPLAY::boundedCommandOutput("sleep 30", std::chrono::milliseconds {300}, &timed_out);
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(output.has_value());
  EXPECT_TRUE(timed_out);
  EXPECT_LT(took, std::chrono::seconds {5});
}

TEST(BoundedCommand, ClosingItsOutputDoesNotLetACommandRunOn) {
  // A layout command is run with its output discarded, so the pipe is at
  // end-of-file from the start; the deadline has to hold on the process.
  bool timed_out = false;
  const auto started = std::chrono::steady_clock::now();
  const auto output = VDISPLAY::boundedCommandOutput("sleep 30 >/dev/null 2>&1", std::chrono::milliseconds {300}, &timed_out);

  EXPECT_FALSE(output.has_value());
  EXPECT_TRUE(timed_out);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds {5});
}

TEST(BoundedCommand, ADescendantHoldingThePipeIsNotWaitedFor) {
  const auto started = std::chrono::steady_clock::now();
  const auto output = VDISPLAY::boundedCommandOutput("echo done; sleep 30 &", std::chrono::seconds {10});

  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(*output, "done\n");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds {5});
}
