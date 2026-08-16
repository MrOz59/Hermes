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
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 1600, 1068, 90, serial, argument)
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
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateNotPlaced, "Virtual-1", 1600, 1068, 90, serial, argument)
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
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 90, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@89.991'"), std::string::npos);

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 120, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@119.982'"), std::string::npos);

  ASSERT_TRUE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateManyRefresh, "Virtual-1", 1600, 1068, 60, serial, argument)
  );
  EXPECT_NE(argument.find("'1600x1068@59.990'"), std::string::npos);
}

TEST(MutterModePush, DoesNothingWhenTheModeIsAlreadyCurrent) {
  std::string serial;
  std::string argument;

  // 1920x1080 is what Mutter already drives - there is nothing to apply.
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 1920, 1080, 60, serial, argument)
  );
  EXPECT_TRUE(argument.empty());
}

TEST(MutterModePush, RefusesGeometryTheConnectorDoesNotAdvertise) {
  std::string serial;
  std::string argument;

  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 3840, 2160, 60, serial, argument)
  );
  EXPECT_TRUE(argument.empty());
}

TEST(MutterModePush, RejectsUnusableInput) {
  std::string serial;
  std::string argument;

  // Connector Mutter has not probed yet - the caller retries rather than fails.
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-7", 1600, 1068, 90, serial, argument)
  );
  EXPECT_FALSE(VDISPLAY::buildMutterLayoutWithMode("", "Virtual-1", 1600, 1068, 90, serial, argument));
  EXPECT_FALSE(
    VDISPLAY::buildMutterLayoutWithMode(kCurrentStateWrongMode, "Virtual-1", 0, 0, 90, serial, argument)
  );
}
