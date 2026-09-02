/**
 * @file tests/unit/platform/test_virtual_display_mutter.cpp
 * @brief Test the Mutter ApplyMonitorsConfig payload built by src/platform/linux/virtual_display.*.
 *
 * A boot-enabled Hermes-KMS connector leaves GNOME extending the desktop onto a
 * black virtual output. Repairing that means submitting an all-or-nothing
 * ApplyMonitorsConfig, so the payload is built from a captured
 * org.gnome.Mutter.DisplayConfig GetCurrentState reply and checked here rather
 * than against a live compositor.
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

namespace {

  /**
   * A GetCurrentState reply shaped exactly like the one gdbus prints, including
   * the quirks that break naive parsing: the serial carries a `uint32` prefix,
   * the transform is annotated on the first logical monitor and bare on the
   * second, empty dicts appear both as `@a{sv} {}` and as `{}`, and mode
   * properties nest variants (`<'variable'>`). Mode lists are shortened; the
   * structure is verbatim.
   */
  constexpr const char *kCurrentState =
    "(uint32 2, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('4096x2160@100.000', 4096, 2160, 100.0, 1.3333333730697632, [1.0, 2.0], @a{sv} {}), "
    "('4096x2160@100.000+vrr', 4096, 2160, 100.0, 1.3333333730697632, [1.0, 2.0], {'refresh-rate-mode': <'variable'>}), "
    "('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], "
    "{'is-builtin': <false>, 'display-name': <'ViewSonic 29\\\"'>}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('2560x1600@59.987', 2560, 1600, 59.986587524414062, 1.0, [1.0, 2.0], {}), "
    "('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], "
    "{'is-builtin': <false>, 'display-name': <'HRM'>})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {}), "
    "(2560, 0, 1.0, 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>, 'supports-changing-layout-mode': <true>})";

  /** The same reply with the virtual output holding the primary flag. */
  constexpr const char *kCurrentStateVirtualPrimary =
    "(uint32 7, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, false, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(2560, 0, 1.0, uint32 0, true, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** One logical monitor mirroring the physical and the virtual output. */
  constexpr const char *kCurrentStateMirrored =
    "(uint32 3, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** Nothing but the virtual output - repairing this would black out the host. */
  constexpr const char *kCurrentStateVirtualOnly =
    "(uint32 4, "
    "[(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /**
   * The situation from the bug report: Hermes asked for 1600x1068@90, the
   * connector advertises it, but Mutter adopted the output at its own preferred
   * 1920x1080@60. Hermes then captures 1600x1068 while Mutter scans out
   * 1920x1080 and the client receives a black image.
   */
  constexpr const char *kCurrentStateWrongMode =
    "(uint32 2, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0, 2.0], {}), "
    "('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], {'display-name': <'HRM'>})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {}), "
    "(2560, 0, 1.0, 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** Mutter probed the connector but has not placed it in the layout yet. */
  constexpr const char *kCurrentStateNotPlaced =
    "(uint32 9, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {})], "
    "{'layout-mode': <uint32 1>})";

  /** Same geometry offered at several refresh rates. */
  constexpr const char *kCurrentStateManyRefresh =
    "(uint32 4, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@59.990', 1600, 1068, 59.99, 1.0, [1.0], {}), "
    "('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {}), "
    "('1600x1068@119.982', 1600, 1068, 119.982, 1.0, [1.0], {}), "
    "('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {}), "
    "(2560, 0, 1.0, 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /**
   * A desktop scaled to 200%: the physical monitor runs a 3840x2160 mode but
   * occupies 1920x1080 of the layout, which is the space the pointer is
   * measured in. Reading the mode size instead would put the envelope at 5440
   * rather than 3520.
   */
  constexpr const char *kCurrentStateScaledDesktop =
    "(uint32 11, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('3840x2160@60.000', 3840, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1920, 0, 1.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** The same desktop with the virtual output itself scaled to 200%. */
  constexpr const char *kCurrentStateScaledVirtual =
    "(uint32 12, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('3840x2160@60.000', 3840, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('3200x2160@60.000', 3200, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {})], "
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1920, 0, 2.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** A monitor rotated 90 degrees: its layout rectangle has the axes swapped. */
  constexpr const char *kCurrentStateRotated =
    "(uint32 13, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 1, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1080, 0, 1.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /**
   * The physical layout mode, where a logical monitor measures the mode itself
   * whatever the scale - so nothing is divided and nothing is converted back.
   */
  constexpr const char *kCurrentStatePhysicalLayout =
    "(uint32 14, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('3840x2160@60.000', 3840, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {})], "
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(3840, 0, 2.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 2>})";

  /** A scaled desktop where Mutter has not placed the connector yet. */
  constexpr const char *kCurrentStateScaledNotPlaced =
    "(uint32 15, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('3840x2160@60.000', 3840, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {})], {})], "
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** The same, on a backend that demands one scale for the whole desktop. */
  constexpr const char *kCurrentStateScaledNotPlacedGlobalScale =
    "(uint32 16, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('3840x2160@60.000', 3840, 2160, 60.0, 2.0, [1.0, 2.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {})], {})], "
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {})], "
    "{'layout-mode': <uint32 1>, 'global-scale-required': <true>})";

  /**
   * The layout from issue #22, reduced from the reporter's own GetCurrentState:
   * a 2560x1080 ViewSonic at the origin with the Hermes output beside it at the
   * 1600x1068 the tablet negotiated. Absolute input measured against the output
   * alone spans the whole 4160-wide desktop instead of the right-hand 1600 of
   * it, which puts most of the client's screen on the physical monitor.
   */
  constexpr const char *kCurrentStateIssue22 =
    "(uint32 2, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('4096x2160@100.000', 4096, 2160, 100.0, 1.3333333730697632, "
    "[1.0, 1.3333333730697632, 2.0, 2.6666667461395264, 4.0], @a{sv} {}), "
    "('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], "
    "{'is-builtin': <false>}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], {'display-name': <'HRM'>})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {}), "
    "(2560, 0, 1.0, 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>, 'supports-changing-layout-mode': <true>})";

  /**
   * A desktop where the virtual output advertises the physical monitor's
   * current resolution, which is the only case GNOME can mirror: cloning means
   * one logical monitor holding both connectors, and Mutter refuses that unless
   * the modes have identical dimensions.
   */
  constexpr const char *kCurrentStateMirrorable =
    "(uint32 21, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@59.950', 1920, 1080, 59.95, 1.0, [1.0], {}), "
    "('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1920, 0, 1.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** A monitor carrying no EDID identity at all, which binds nothing. */
  constexpr const char *kCurrentStateNamelessMonitor =
    "(uint32 31, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', '', '', ''), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1920, 0, 1.0, uint32 0, false, [('Virtual-1', '', '', '')], {})], "
    "{'layout-mode': <uint32 1>})";

  /** A monitor whose EDID carries characters a shell would act on. */
  constexpr const char *kCurrentStateHostileEdid =
    "(uint32 32, "
    "[(('Virtual-1', 'HRM$(id)', 'Hermes KMS', '0x00000001'), "
    "[('1600x1068@89.991', 1600, 1068, 89.990867614746094, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('Virtual-1', 'HRM$(id)', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

}  // namespace

TEST(MutterLayoutRepair, DropsTheVirtualLogicalMonitor) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentState, "Virtual-1", serial, argument));
  EXPECT_EQ(serial, "2");
  // The physical monitor keeps its position, scale and mode; the config carries
  // no trailing property dict, which is what ApplyMonitorsConfig expects.
  EXPECT_EQ(argument, "[(0, 0, 1.0, uint32 0, true, [('DP-2', '2560x1080@59.896', @a{sv} {})])]");
}

TEST(MutterLayoutRepair, MovesPrimaryToTheSurvivingMonitor) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentStateVirtualPrimary, "Virtual-1", serial, argument)
  );
  EXPECT_EQ(serial, "7");
  EXPECT_EQ(argument, "[(0, 0, 1.0, uint32 0, true, [('DP-2', '2560x1080@59.896', @a{sv} {})])]");
}

TEST(MutterLayoutRepair, KeepsThePhysicalHalfOfAMirroredMonitor) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentStateMirrored, "Virtual-1", serial, argument));
  EXPECT_EQ(serial, "3");
  EXPECT_EQ(argument, "[(0, 0, 1.0, uint32 0, true, [('DP-2', '1920x1080@60.000', @a{sv} {})])]");
}

TEST(MutterLayoutRepair, DoesNothingWhenTheConnectorIsNotInTheLayout) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentState, "Virtual-9", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterLayoutRepair, RefusesToLeaveMutterWithoutAMonitor) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentStateVirtualOnly, "Virtual-1", serial, argument)
  );
  EXPECT_TRUE(argument.empty());
}

TEST(MutterLayoutRepair, RejectsUnusableReplies) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector("", "Virtual-1", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector("not a variant", "Virtual-1", serial, argument));
  // Truncated mid-array: the reply parses as a tuple but has no logical monitors.
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector("(uint32 2, [], [], {})", "Virtual-1", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector(kCurrentState, "", serial, argument));
}

TEST(MutterLayoutRepair, RefusesConnectorNamesThatCouldEscapeTheShell) {
  // The payload is interpolated into a gdbus command line, so a connector name
  // carrying shell metacharacters must abort the repair instead of being quoted
  // and hoped for.
  const std::string hostile =
    "(uint32 5, "
    "[(('DP-2; touch /tmp/pwned', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0], {'is-current': <true>})], {})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2; touch /tmp/pwned', 'VSC', 'VA2932 SERIES', 'WL5242501007')], {}), "
    "(1920, 0, 1.0, uint32 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>})";

  std::string serial;
  std::string argument;
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithoutConnector(hostile, "Virtual-1", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterModePush, SetsTheModeTheClientAskedFor) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 1600, 1068, 90000, serial, argument)
  );
  EXPECT_EQ(serial, "2");
  // DP-2 keeps its mode and position; Virtual-1 moves off Mutter's preferred
  // 1920x1080 onto the geometry Hermes is capturing.
  EXPECT_EQ(
    argument,
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', '2560x1080@59.896', @a{sv} {})]), "
    "(2560, 0, 1.0, uint32 0, false, [('Virtual-1', '1600x1068@89.991', @a{sv} {})])]"
  );
}

TEST(MutterModePush, PlacesAnUnplacedOutputAtTheRightEdge) {
  std::string serial;
  std::string argument;

  // Mutter rejects layouts with gaps, so the appended monitor must start
  // exactly where the 2560-wide DP-2 ends.
  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateNotPlaced, "Virtual-1", 1600, 1068, 90000, serial, argument)
  );
  EXPECT_EQ(serial, "9");
  EXPECT_EQ(
    argument,
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', '2560x1080@59.896', @a{sv} {})]), "
    "(2560, 0, 1.0, uint32 0, false, [('Virtual-1', '1600x1068@89.991', @a{sv} {})])]"
  );
}

TEST(MutterModePush, PicksTheClosestRefreshRate) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 90000, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@89.991'"), std::string::npos);

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 120000, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@119.982'"), std::string::npos);

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 60000, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@59.990'"), std::string::npos);
}

TEST(MutterModePush, DoesNothingWhenTheModeIsAlreadyCurrent) {
  std::string serial;
  std::string argument;

  // 1920x1080 is what Mutter already drives - there is nothing to apply.
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 1920, 1080, 60000, serial, argument)
  );
  EXPECT_TRUE(argument.empty());
}

TEST(MutterModePush, RefusesGeometryTheConnectorDoesNotAdvertise) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 3840, 2160, 60000, serial, argument)
  );
  EXPECT_TRUE(argument.empty());
}

TEST(MutterModePush, RejectsUnusableInput) {
  std::string serial;
  std::string argument;

  // Connector Mutter has not probed yet - the caller retries rather than fails.
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-7", 1600, 1068, 90000, serial, argument)
  );
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithMode("", "Virtual-1", 1600, 1068, 90000, serial, argument));
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 0, 0, 90000, serial, argument)
  );
}

TEST(MutterDisplayGeometry, PlacesTheVirtualOutputOnTheDesktop) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(VDISPLAY::mutterDisplayGeometry(kCurrentState, "Virtual-1", x, y, env_width, env_height));
  // The virtual output sits to the right of a 2560x1080 monitor, so absolute
  // input has to be measured against the 4480-wide desktop, not against 1920.
  EXPECT_EQ(x, 2560);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 4480);
  EXPECT_EQ(env_height, 1080);
}

TEST(MutterDisplayGeometry, MeasuresAScaledDesktopInLayoutPixels) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(
    VDISPLAY::mutterDisplayGeometry(kCurrentStateScaledDesktop, "Virtual-1", x, y, env_width, env_height)
  );
  // The physical monitor's 3840x2160 mode occupies 1920x1080 of the layout at
  // 200%. Taking the mode size would put the envelope at 5440x2160 and send the
  // pointer to roughly half of where the client aimed.
  EXPECT_EQ(x, 1920);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 3520);
  EXPECT_EQ(env_height, 1080);
}

TEST(MutterDisplayGeometry, ReturnsTheVirtualOutputsOwnPixelSpace) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(
    VDISPLAY::mutterDisplayGeometry(kCurrentStateScaledVirtual, "Virtual-1", x, y, env_width, env_height)
  );
  // The layout is 3520x1080 logical with the output at 1920,0; scaling both by
  // the output's own 200% keeps the ratio the consumer forms while letting the
  // client's coordinates stay in the captured mode's 3200x2160 pixels.
  EXPECT_EQ(x, 3840);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 7040);
  EXPECT_EQ(env_height, 2160);
  // The ratio is what actually reaches the pointer, so state it directly.
  EXPECT_DOUBLE_EQ(static_cast<double>(x) / env_width, 1920.0 / 3520.0);
}

TEST(MutterDisplayGeometry, SwapsTheAxesOfARotatedMonitor) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(VDISPLAY::mutterDisplayGeometry(kCurrentStateRotated, "Virtual-1", x, y, env_width, env_height));
  // The 1920x1080 monitor is rotated 90 degrees, so it occupies 1080x1920.
  EXPECT_EQ(x, 1080);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 2680);
  EXPECT_EQ(env_height, 1920);
}

TEST(MutterDisplayGeometry, LeavesThePhysicalLayoutModeAlone) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(
    VDISPLAY::mutterDisplayGeometry(kCurrentStatePhysicalLayout, "Virtual-1", x, y, env_width, env_height)
  );
  // layout-mode 2 counts mode pixels whatever the scale, so a 200% desktop must
  // not be divided on the way in nor multiplied on the way out.
  EXPECT_EQ(x, 3840);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 5760);
  EXPECT_EQ(env_height, 2160);
}

TEST(MutterDisplayGeometry, ReportsAMirroredOutputAtTheSharedPosition) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(VDISPLAY::mutterDisplayGeometry(kCurrentStateMirrored, "Virtual-1", x, y, env_width, env_height));
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 1920);
  EXPECT_EQ(env_height, 1080);
}

TEST(MutterDisplayGeometry, RefusesWhatItCannotMeasure) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  // Probed but not placed in the layout: there is no offset to report, and
  // guessing one would aim the pointer at a rectangle the desktop does not have.
  EXPECT_FALSE(
    VDISPLAY::mutterDisplayGeometry(kCurrentStateNotPlaced, "Virtual-1", x, y, env_width, env_height)
  );
  EXPECT_FALSE(VDISPLAY::mutterDisplayGeometry(kCurrentState, "Virtual-9", x, y, env_width, env_height));
  EXPECT_FALSE(VDISPLAY::mutterDisplayGeometry(kCurrentState, "", x, y, env_width, env_height));
  EXPECT_FALSE(VDISPLAY::mutterDisplayGeometry("", "Virtual-1", x, y, env_width, env_height));
  EXPECT_FALSE(VDISPLAY::mutterDisplayGeometry("not a variant", "Virtual-1", x, y, env_width, env_height));
}

TEST(MutterLayoutMode, PlacesANewMonitorAtTheLogicalRightEdge) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterLayoutWithMode(
    kCurrentStateScaledNotPlaced, "Virtual-1", 1600, 1068, 90000, serial, argument
  ));
  EXPECT_EQ(serial, "15");
  // 1920, not 3840: the 200% monitor's right edge is in layout pixels. A gap
  // there is not a cosmetic problem - Mutter refuses a layout whose monitors
  // are not all adjacent, and refuses the whole config with it.
  EXPECT_EQ(
    argument,
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', '3840x2160@60.000', @a{sv} {})]), "
    "(1920, 0, 1.0, uint32 0, false, [('Virtual-1', '1600x1068@89.991', @a{sv} {})])]"
  );
}

TEST(MutterLayoutMode, AdoptsTheDesktopScaleWhenOneIsRequired) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterLayoutWithMode(
    kCurrentStateScaledNotPlacedGlobalScale, "Virtual-1", 1600, 1068, 90000, serial, argument
  ));
  EXPECT_EQ(serial, "16");
  // A scale of our own would be refused with "Logical monitor scales must be
  // identical", taking the whole config down.
  EXPECT_EQ(
    argument,
    "[(0, 0, 2.0, uint32 0, true, [('DP-2', '3840x2160@60.000', @a{sv} {})]), "
    "(1920, 0, 2.0, uint32 0, false, [('Virtual-1', '1600x1068@89.991', @a{sv} {})])]"
  );
}

TEST(MutterDisplayGeometry, MatchesTheLayoutFromIssue22) {
  int x = -1;
  int y = -1;
  int env_width = -1;
  int env_height = -1;

  ASSERT_TRUE(VDISPLAY::mutterDisplayGeometry(kCurrentStateIssue22, "Virtual-1", x, y, env_width, env_height));
  EXPECT_EQ(x, 2560);
  EXPECT_EQ(y, 0);
  EXPECT_EQ(env_width, 4160);
  EXPECT_EQ(env_height, 1080);
  // Without this, the offset is 0 and the envelope is the output's own
  // 1600x1068, so the client's screen stretches across the whole desktop and
  // its left-hand 61% lands on the physical monitor.
  EXPECT_GT(env_width, 1600);
}

TEST(MutterExclusiveLayout, HandsTheDesktopToTheVirtualOutput) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterExclusiveLayout(kCurrentState, "Virtual-1", serial, argument));
  EXPECT_EQ(serial, "2");
  // The physical monitor is turned off by being left out: Mutter disables every
  // connected monitor a config does not name. It is also why this has to be one
  // payload - there is no per-output disable to call.
  EXPECT_EQ(argument, "[(0, 0, 1.0, uint32 0, true, [('Virtual-1', '1920x1080@60.000', @a{sv} {})])]");
}

TEST(MutterExclusiveLayout, AnchorsTheOutputAtTheOrigin) {
  std::string serial;
  std::string argument;

  // The virtual output sits at x=2560 in the state; a config whose monitors do
  // not start at 0,0 is rejected with "Logical monitors positions are offset".
  ASSERT_TRUE(VDISPLAY::buildMutterExclusiveLayout(kCurrentStateIssue22, "Virtual-1", serial, argument));
  EXPECT_EQ(argument, "[(0, 0, 1.0, uint32 0, true, [('Virtual-1', '1600x1068@89.991', @a{sv} {})])]");
}

TEST(MutterExclusiveLayout, KeepsTheScaleMutterIsAlreadyDriving) {
  std::string serial;
  std::string argument;

  // 3200x2160 at 200%. Asking for a scale of our own can fail Mutter's "mode
  // divided by scale must be a whole number" check.
  ASSERT_TRUE(VDISPLAY::buildMutterExclusiveLayout(kCurrentStateScaledVirtual, "Virtual-1", serial, argument));
  EXPECT_EQ(argument, "[(0, 0, 2.0, uint32 0, true, [('Virtual-1', '3200x2160@60.000', @a{sv} {})])]");
}

TEST(MutterExclusiveLayout, DoesNothingWhenTheOutputAlreadyHasTheDesktop) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterExclusiveLayout(kCurrentStateVirtualOnly, "Virtual-1", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterExclusiveLayout, RefusesToBlankTheDesktopForAnUndrivenOutput) {
  std::string serial;
  std::string argument;

  // Probed but not being scanned out: there is no mode to name, and turning the
  // physical monitors off for it would leave nothing on screen at all.
  EXPECT_FALSE(VDISPLAY::buildMutterExclusiveLayout(kCurrentStateNotPlaced, "Virtual-1", serial, argument));
  EXPECT_TRUE(argument.empty());

  EXPECT_FALSE(VDISPLAY::buildMutterExclusiveLayout(kCurrentState, "Virtual-9", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterExclusiveLayout("", "Virtual-1", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterExclusiveLayout(kCurrentState, "", serial, argument));
}

TEST(MutterMirrorLayout, PutsBothOutputsInOneLogicalMonitor) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterMirrorLayout(kCurrentStateMirrorable, "Virtual-1", serial, argument));
  EXPECT_EQ(serial, "21");
  // One logical monitor, two connectors, both at 1920x1080 - and the virtual
  // output's own logical monitor is gone, so nothing is left floating at 1920,0
  // where Mutter would find a gap.
  EXPECT_EQ(
    argument,
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', '1920x1080@60.000', @a{sv} {}), "
    "('Virtual-1', '1920x1080@59.950', @a{sv} {})])]"
  );
}

TEST(MutterMirrorLayout, RefusesWhenNoSharedModeSizeExists) {
  std::string serial;
  std::string argument;

  // The physical monitor runs 2560x1080; the virtual output advertises
  // 2560x1600 and 1920x1080 but nothing 2560x1080, and Mutter rejects a logical
  // monitor whose monitors' modes differ in size.
  EXPECT_FALSE(VDISPLAY::buildMutterMirrorLayout(kCurrentState, "Virtual-1", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterMirrorLayout, DoesNothingWhenAlreadyMirrored) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterMirrorLayout(kCurrentStateMirrored, "Virtual-1", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterMirrorLayout, RefusesWhenThereIsNothingToMirror) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterMirrorLayout(kCurrentStateVirtualOnly, "Virtual-1", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterMirrorLayout("", "Virtual-1", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterMirrorLayout(kCurrentState, "", serial, argument));
}

TEST(MutterRestoreLayout, ReproducesTheLayoutItWasGiven) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentState, serial, argument));
  EXPECT_EQ(serial, "2");
  // Every logical monitor, at its position, scale, transform and current mode:
  // this is what a session submits again when it gives the desktop back.
  EXPECT_EQ(
    argument,
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', '2560x1080@59.896', @a{sv} {})]), "
    "(2560, 0, 1.0, uint32 0, false, [('Virtual-1', '1920x1080@60.000', @a{sv} {})])]"
  );
}

TEST(MutterRestoreLayout, RoundTripsAMirroredLayout) {
  std::string serial;
  std::string argument;

  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentStateMirrored, serial, argument));
  EXPECT_EQ(
    argument,
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', '1920x1080@60.000', @a{sv} {}), "
    "('Virtual-1', '1920x1080@60.000', @a{sv} {})])]"
  );
}

TEST(MutterRestoreLayout, RejectsUnusableReplies) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(VDISPLAY::buildMutterRestoreLayout("", serial, argument));
  EXPECT_FALSE(VDISPLAY::buildMutterRestoreLayout("not a variant", serial, argument));
  EXPECT_TRUE(argument.empty());
}

TEST(MutterInputMapping, BindsADeviceToTheVirtualMonitorsIdentity) {
  // GNOME matches an input device to a monitor by EDID, not by connector, so
  // the value is the monitor's vendor, product and serial - with the connector
  // appended, which Mutter uses when two monitors share an EDID.
  EXPECT_EQ(
    VDISPLAY::mutterInputDeviceOutputValue(kCurrentState, "Virtual-1"),
    "['HRM', 'Hermes KMS', '0x00000001', 'Virtual-1']"
  );
}

TEST(MutterInputMapping, RefusesAMonitorWithNoIdentity) {
  // Three empty strings is how GNOME spells "not configured", so writing it
  // would bind nothing while still counting as a user value - which also stops
  // GNOME from guessing. Leaving the key alone is strictly better.
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue(kCurrentStateNamelessMonitor, "Virtual-1"), "");
}

TEST(MutterInputMapping, RefusesAnEdidThatCouldEscapeTheShell) {
  // The value reaches gsettings through a command line and the EDID comes from
  // the monitor. A name needing escaping is refused rather than escaped; the
  // cost is the fallback placement we already had.
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue(kCurrentStateHostileEdid, "Virtual-1"), "");
}

TEST(MutterInputMapping, RefusesWhatItCannotIdentify) {
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue(kCurrentState, "Virtual-9"), "");
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue(kCurrentState, ""), "");
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue("", "Virtual-1"), "");
  EXPECT_EQ(VDISPLAY::mutterInputDeviceOutputValue("not a variant", "Virtual-1"), "");
}

TEST(MutterInputMapping, TargetsTheSettingsPathsGnomeBuildsForOurDevices) {
  std::string touch;
  std::string pen;
  VDISPLAY::mutterInputDeviceSettingsTargets(touch, pen);

  // Mutter builds the path from the device's vendor and product as %.4x:%.4x,
  // and puts touchscreens and the tablet family in different schemas.
  EXPECT_EQ(touch, "org.gnome.desktop.peripherals.touchscreen:/org/gnome/desktop/peripherals/touchscreens/beef:dead/");
  EXPECT_EQ(pen, "org.gnome.desktop.peripherals.tablet:/org/gnome/desktop/peripherals/tablets/beef:dead/");
}


TEST(MutterLayoutWatch, TheRestoreConfigDistinguishesLayoutsThatMoveOutputs) {
  // The layout watch decides whether a change matters by comparing the config a
  // restore would submit. That only works while the config actually differs for
  // the changes the capture path cares about, which is what this pins: a moved
  // output and a changed mode both alter it.
  std::string serial;
  std::string extended;
  std::string mirrored;
  std::string scaled;

  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentState, serial, extended));
  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentStateMirrored, serial, mirrored));
  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentStateScaledDesktop, serial, scaled));

  EXPECT_NE(extended, mirrored);
  EXPECT_NE(extended, scaled);
  EXPECT_NE(mirrored, scaled);
}

TEST(MutterLayoutWatch, TheRestoreConfigIgnoresWhatDoesNotMoveAnOutput) {
  // The same layout described by a reply carrying a different serial and extra
  // monitor properties has to compare equal, or every unrelated notification -
  // a backlight change, a privacy screen - would reinitialise the capture.
  constexpr const char *same_layout_other_serial =
    "(uint32 99, "
    "[(('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007'), "
    "[('4096x2160@100.000', 4096, 2160, 100.0, 1.3333333730697632, [1.0, 2.0], @a{sv} {}), "
    "('4096x2160@100.000+vrr', 4096, 2160, 100.0, 1.3333333730697632, [1.0, 2.0], "
    "{'refresh-rate-mode': <'variable'>}), "
    "('2560x1080@59.896', 2560, 1080, 59.896186828613281, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], "
    "{'is-builtin': <false>, 'privacy-screen-state': <(false, false)>}), "
    "(('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001'), "
    "[('2560x1600@59.987', 2560, 1600, 59.986587524414062, 1.0, [1.0, 2.0], {}), "
    "('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0, 2.0], "
    "{'is-current': <true>, 'is-preferred': <true>})], "
    "{'is-builtin': <false>, 'display-name': <'HRM'>})], "
    "[(0, 0, 1.0, uint32 0, true, [('DP-2', 'VSC', 'VA2932 SERIES', 'WL5242501007')], @a{sv} {}), "
    "(2560, 0, 1.0, 0, false, [('Virtual-1', 'HRM', 'Hermes KMS', '0x00000001')], {})], "
    "{'layout-mode': <uint32 1>, 'supports-changing-layout-mode': <true>})";

  std::string serial;
  std::string before;
  std::string after;
  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(kCurrentState, serial, before));
  ASSERT_TRUE(VDISPLAY::buildMutterRestoreLayout(same_layout_other_serial, serial, after));
  EXPECT_EQ(before, after);
}
