/**
 * @file tests/unit/platform/test_virtual_display_kscreen.cpp
 * @brief Test the kscreen-doctor invocation built by src/platform/linux/virtual_display.*.
 *
 * KWin adopts a hotplugged Hermes-KMS connector at whichever mode it prefers
 * rather than the one the client negotiated, and the capture path reports the
 * real scanout - so an 854x480 session was streamed as 1920x1080 with every
 * layer below the compositor holding the right geometry. The command is the
 * only place that can correct it, so it is checked here rather than against a
 * live KDE session.
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

#include <map>
#include <string>
#include <vector>

namespace {

  /** A single physical monitor holding the top priority, as KScreen reports it. */
  const std::map<std::string, int> kSinglePhysical {{"DP-2", 1}};

}  // namespace

TEST(KScreenModePush, DrivesTheOutputAtTheModeTheClientAskedFor) {
  // The reported regression: a 480p session that arrived as 1080p because the
  // command enabled and placed the output but never named a mode.
  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 2560, 0, 854, 480, 60),
    "kscreen-doctor"
    " output.DP-2.enable output.DP-2.priority.1"
    " output.Virtual-1.enable output.Virtual-1.priority.2"
    " output.Virtual-1.position.2560,0"
    " output.Virtual-1.mode.854x480@60"
  );
}

TEST(KScreenModePush, OmitsTheModeSoARejectedLayoutCanBeRetried) {
  // The caller retries with a zeroed mode when KWin refuses the first command:
  // an output enabled at the wrong resolution still beats no output at all.
  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 2560, 0, 0, 0, 0),
    "kscreen-doctor"
    " output.DP-2.enable output.DP-2.priority.1"
    " output.Virtual-1.enable output.Virtual-1.priority.2"
    " output.Virtual-1.position.2560,0"
  );
}

TEST(KScreenModePush, OmitsAnIncompleteMode) {
  // A geometry with no refresh rate cannot be expressed as WxH@R, and half a
  // mode argument would fail the whole transaction.
  const auto no_refresh = VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 0, 0, 854, 480, 0);
  EXPECT_EQ(no_refresh.find(".mode."), std::string::npos);

  const auto no_height = VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 0, 0, 854, 0, 60);
  EXPECT_EQ(no_height.find(".mode."), std::string::npos);

  const auto negative = VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 0, 0, -1, 480, 60);
  EXPECT_EQ(negative.find(".mode."), std::string::npos);
}

TEST(KScreenLayout, ReEnablesEveryPreSessionOutputAtItsOwnPriority) {
  // Creating a virtual display must not cost the user their monitors: every
  // output that was lit before the hotplug is re-enabled at the priority it
  // held, and the virtual output takes the next free one.
  const std::map<std::string, int> enabled_before {{"DP-2", 1}, {"HDMI-A-1", 2}};

  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", enabled_before, 0, 0, 1280, 720, 60),
    "kscreen-doctor"
    " output.DP-2.enable output.DP-2.priority.1"
    " output.HDMI-A-1.enable output.HDMI-A-1.priority.2"
    " output.Virtual-1.enable output.Virtual-1.priority.3"
    " output.Virtual-1.position.0,0"
    " output.Virtual-1.mode.1280x720@60"
  );
}

TEST(KScreenLayout, DoesNotEnableTheVirtualOutputTwice) {
  // KWin can report the virtual connector as already enabled from a previous
  // session. Naming it twice in one transaction is not a layout KScreen owes
  // us any particular behaviour for, so it is filtered out of the replay.
  const std::map<std::string, int> enabled_before {{"DP-2", 1}, {"Virtual-1", 5}};
  const auto command = VDISPLAY::buildKScreenLayoutCommand("Virtual-1", enabled_before, 0, 0, 854, 480, 60);

  EXPECT_EQ(command.find("output.Virtual-1.priority.5"), std::string::npos);
  EXPECT_EQ(
    command,
    "kscreen-doctor"
    " output.DP-2.enable output.DP-2.priority.1"
    " output.Virtual-1.enable output.Virtual-1.priority.2"
    " output.Virtual-1.position.0,0"
    " output.Virtual-1.mode.854x480@60"
  );
}

TEST(KScreenLayout, LeavesPriorityUnsetForAnOutputThatHasNone) {
  // KScreen reports 0 for an output outside the priority order. Replaying that
  // as `.priority.0` would be a different request from the one we captured.
  const std::map<std::string, int> enabled_before {{"DP-2", 0}};

  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", enabled_before, 0, 0, 854, 480, 60),
    "kscreen-doctor"
    " output.DP-2.enable"
    " output.Virtual-1.enable output.Virtual-1.priority.1"
    " output.Virtual-1.position.0,0"
    " output.Virtual-1.mode.854x480@60"
  );
}

TEST(KScreenLayout, PlacesTheOutputWhereTheCallerComputedIt) {
  // The position keeps the virtual desktop regions disjoint; a mirrored region
  // would put physical content into the stream and make absolute input
  // ambiguous. Negative origins are legitimate in a multi-monitor layout.
  EXPECT_NE(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, -1920, -180, 854, 480, 60)
      .find(" output.Virtual-1.position.-1920,-180"),
    std::string::npos
  );
}

TEST(KScreenLayout, WorksWithNoPreSessionOutputs) {
  // A headless host has nothing to replay, and the virtual output still has to
  // come up as the first priority.
  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", {}, 0, 0, 1920, 1080, 120),
    "kscreen-doctor"
    " output.Virtual-1.enable output.Virtual-1.priority.1"
    " output.Virtual-1.position.0,0"
    " output.Virtual-1.mode.1920x1080@120"
  );
}

namespace {

  /** The desktop as KScreen reported it before the connector was hotplugged. */
  const std::vector<VDISPLAY::kscreen_output_t> kPhysicalOnly {
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 0, .width = 1920, .height = 1080},
  };

  /**
   * What KWin shows during the hotplug: it replays the setup saved for this
   * output combination, which an exclusive session left with the monitor
   * disabled and the virtual connector at the origin.
   */
  const std::vector<VDISPLAY::kscreen_output_t> kReplayedExclusiveSetup {
    {.name = "HDMI-A-1", .connected = true, .enabled = false, .priority = 2, .x = 0, .y = 0, .width = 1920, .height = 1080},
    {.name = "Virtual-1", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 0, .width = 854, .height = 480},
  };

}  // namespace

TEST(KScreenPlacement, PlacesBesideAMonitorKWinHasTemporarilyDisabled) {
  // The reported bug: KWin replayed a saved exclusive setup on the hotplug, the
  // placement read that state, found no enabled output to append to and left
  // the virtual output at 0,0. The same transaction re-enabled the monitor at
  // the origin, so both sat there and the client received the physical desktop
  // cropped to the mode it had asked for.
  const auto position = VDISPLAY::kscreenVirtualOutputPosition(
    "Virtual-1", kPhysicalOnly, kReplayedExclusiveSetup, VDISPLAY::virtual_display_layout_e::extend);
  EXPECT_EQ(position.x, 1920);
  EXPECT_EQ(position.y, 0);
}

TEST(KScreenPlacement, AppendsToTheRightOfEveryOutputBeingRestored) {
  const std::vector<VDISPLAY::kscreen_output_t> before {
    {.name = "DP-2", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 120, .width = 2560, .height = 1080},
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 2, .x = 2560, .y = 0, .width = 1920, .height = 1080},
    {.name = "DVI-I-1", .connected = true, .enabled = false, .priority = 3, .x = 4480, .y = 0, .width = 1920, .height = 1080},
  };
  const auto position = VDISPLAY::kscreenVirtualOutputPosition(
    "Virtual-1", before, before, VDISPLAY::virtual_display_layout_e::extend);
  // Past the rightmost enabled output, and level with the topmost of them - the
  // disabled one is not being restored, so it reserves no space.
  EXPECT_EQ(position.x, 4480);
  EXPECT_EQ(position.y, 0);
}

TEST(KScreenPlacement, IgnoresAnOutputThatHasSinceBeenUnplugged) {
  // A monitor that left between the snapshot and the hotplug is not coming
  // back, so reserving its width would strand the virtual output in a gap.
  const std::vector<VDISPLAY::kscreen_output_t> before {
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 0, .width = 1920, .height = 1080},
    {.name = "DP-2", .connected = true, .enabled = true, .priority = 2, .x = 1920, .y = 0, .width = 2560, .height = 1080},
  };
  const std::vector<VDISPLAY::kscreen_output_t> current {
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 0, .width = 1920, .height = 1080},
    {.name = "Virtual-1", .connected = true, .enabled = true, .priority = 2, .x = 0, .y = 0, .width = 854, .height = 480},
  };
  EXPECT_EQ(
    VDISPLAY::kscreenVirtualOutputPosition("Virtual-1", before, current, VDISPLAY::virtual_display_layout_e::extend).x,
    1920
  );
}

TEST(KScreenPlacement, FallsBackToTheCurrentLayoutWithoutASnapshot) {
  // kscreen-doctor gave no answer before the hotplug - a session that came up
  // with the compositor still assembling its outputs. The live layout is then
  // the only thing to place against, and it still beats the origin.
  const std::vector<VDISPLAY::kscreen_output_t> current {
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 1, .x = 0, .y = 0, .width = 1920, .height = 1080},
    {.name = "Virtual-1", .connected = true, .enabled = true, .priority = 2, .x = 0, .y = 0, .width = 854, .height = 480},
  };
  EXPECT_EQ(
    VDISPLAY::kscreenVirtualOutputPosition("Virtual-1", {}, current, VDISPLAY::virtual_display_layout_e::extend).x,
    1920
  );
}

TEST(KScreenPlacement, MirrorAnchorsOnThePrimaryOutput) {
  // Mirror deliberately overlaps, so the desktop is cloned onto the virtual
  // connector; it anchors on the primary rather than on the first output seen.
  const std::vector<VDISPLAY::kscreen_output_t> before {
    {.name = "DP-2", .connected = true, .enabled = true, .priority = 2, .x = 0, .y = 0, .width = 2560, .height = 1080},
    {.name = "HDMI-A-1", .connected = true, .enabled = true, .priority = 1, .x = 2560, .y = 180, .width = 1920, .height = 1080},
  };
  const auto position = VDISPLAY::kscreenVirtualOutputPosition(
    "Virtual-1", before, before, VDISPLAY::virtual_display_layout_e::mirror);
  EXPECT_EQ(position.x, 2560);
  EXPECT_EQ(position.y, 180);
}

TEST(KScreenLayout, PutsTheRestoredOutputsBackWhereTheyWere) {
  // Enabling an output does not say where it goes, so KWin kept the position
  // from the setup it replayed and a monitor came back on top of the virtual
  // output. Naming the position makes the transaction the whole layout.
  const std::map<std::string, VDISPLAY::kscreen_point_t> positions {{"DP-2", {0, 120}}};
  EXPECT_EQ(
    VDISPLAY::buildKScreenLayoutCommand("Virtual-1", kSinglePhysical, 2560, 0, 854, 480, 60, positions),
    "kscreen-doctor"
    " output.DP-2.enable output.DP-2.priority.1 output.DP-2.position.0,120"
    " output.Virtual-1.enable output.Virtual-1.priority.2"
    " output.Virtual-1.position.2560,0"
    " output.Virtual-1.mode.854x480@60"
  );
}

TEST(KScreenLayout, RefusesOutputNamesThatCouldEscapeTheShell) {
  // The command is handed to a shell. Output names come from KWin and from
  // sysfs, so this is a statement of that assumption rather than a defence
  // against it - but an unchecked name is one bug upstream away from being a
  // command, and the caller treats an empty invocation as a refusal.
  EXPECT_EQ(VDISPLAY::buildKScreenLayoutCommand("Virtual-1; rm -rf /", kSinglePhysical, 0, 0, 854, 480, 60), "");
  EXPECT_EQ(VDISPLAY::buildKScreenLayoutCommand("$(id)", kSinglePhysical, 0, 0, 854, 480, 60), "");
  EXPECT_EQ(VDISPLAY::buildKScreenLayoutCommand("", kSinglePhysical, 0, 0, 854, 480, 60), "");

  // A bad name among the outputs being replayed costs that output, not the
  // session: the virtual display still has to come up.
  const std::map<std::string, int> tainted {{"DP-2", 1}, {"DP-2 && reboot", 2}};
  const auto command = VDISPLAY::buildKScreenLayoutCommand("Virtual-1", tainted, 0, 0, 854, 480, 60);
  EXPECT_EQ(command.find("reboot"), std::string::npos);
  EXPECT_NE(command.find("output.DP-2.enable"), std::string::npos);
  EXPECT_NE(command.find("output.Virtual-1.mode.854x480@60"), std::string::npos);
}

namespace {

  /** A `kscreen-doctor -j` reply shaped like the one KWin returns. */
  const std::string kModeReply = R"({"outputs":[
    {"name":"DP-2","connected":true,"enabled":true,"currentModeId":"9",
     "modes":[{"id":"9","name":"2560x1080@60","refreshRate":59.895694,"size":{"width":2560,"height":1080}}]},
    {"name":"Virtual-1","connected":true,"enabled":true,"currentModeId":"4",
     "modes":[
       {"id":"3","name":"1920x1080@60","refreshRate":60.0,"size":{"width":1920,"height":1080}},
       {"id":"4","name":"1600x1068@90","refreshRate":89.990867,"size":{"width":1600,"height":1068}}]}
  ]})";

}  // namespace

TEST(KScreenModeState, SeesTheModeKWinIsDriving) {
  // The case that saves a kscreen-doctor call per session, and the one the
  // post-apply check reads to confirm the request was kept.
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "Virtual-1", 1600, 1068, 90),
    VDISPLAY::kscreen_mode_state_e::current
  );
}

TEST(KScreenModeState, RoundsTheRefreshRateTheWayKScreenDoctorDoes) {
  // The connector advertises 89.990867 Hz and is driven by asking for 90:
  // kscreen-doctor rounds when it matches a WxH@R argument. Comparing exactly
  // would report a mode the output plainly has as not advertised - the mHz/Hz
  // class of bug that already cost a release on the Mutter path.
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "DP-2", 2560, 1080, 60),
    VDISPLAY::kscreen_mode_state_e::current
  );
}

TEST(KScreenModeState, SeparatesAdvertisedFromNotAdvertised) {
  // Both used to arrive as one generic command failure. Advertised means "ask
  // KWin for it"; not advertised means the request can only ever fail, and the
  // log should say which.
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "Virtual-1", 1920, 1080, 60),
    VDISPLAY::kscreen_mode_state_e::advertised
  );
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "Virtual-1", 854, 480, 60),
    VDISPLAY::kscreen_mode_state_e::not_advertised
  );
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "Virtual-1", 1600, 1068, 60),
    VDISPLAY::kscreen_mode_state_e::not_advertised
  );
}

TEST(KScreenModeState, TreatsAnUnreadableReplyAsNoAnswer) {
  // An output KWin has not enumerated yet, and a reply that is not JSON at all,
  // must both leave the caller free to try anyway: neither is evidence that the
  // mode was refused.
  EXPECT_EQ(
    VDISPLAY::kscreenModeState(kModeReply, "Virtual-2", 1600, 1068, 90),
    VDISPLAY::kscreen_mode_state_e::unknown_output
  );
  EXPECT_EQ(
    VDISPLAY::kscreenModeState("kscreen-doctor: could not connect", "Virtual-1", 1600, 1068, 90),
    VDISPLAY::kscreen_mode_state_e::unknown_output
  );
  EXPECT_EQ(
    VDISPLAY::kscreenModeState("", "Virtual-1", 1600, 1068, 90),
    VDISPLAY::kscreen_mode_state_e::unknown_output
  );
}
