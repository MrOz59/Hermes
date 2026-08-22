/**
 * @file tests/unit/platform/test_virtual_display_compositor.cpp
 * @brief Test the compositor classification in src/platform/linux/virtual_display.*.
 *
 * The virtual-display strategy differs per compositor in kind - KWin is
 * configured through kscreen-doctor, Mutter only through ApplyMonitorsConfig,
 * Hyprland accepts the output and then composites nothing onto it - so
 * everything downstream branches on this one answer. It is derived from an
 * environment variable whose contents are a distribution's choice, which is
 * exactly the kind of input that is easier to test than to trust.
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

#include <string>

TEST(CompositorDetection, RecognisesTheSupportedCompositors) {
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("KDE"), VDISPLAY::compositor_e::kwin);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("GNOME"), VDISPLAY::compositor_e::mutter);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("Hyprland"), VDISPLAY::compositor_e::hyprland);
}

TEST(CompositorDetection, IgnoresCase) {
  // XDG_CURRENT_DESKTOP casing is not guaranteed: GNOME sessions have shipped
  // both "GNOME" and "gnome", which is what the old substring check had to
  // spell out twice.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("kde"), VDISPLAY::compositor_e::kwin);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("gnome"), VDISPLAY::compositor_e::mutter);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("hyprland"), VDISPLAY::compositor_e::hyprland);
}

TEST(CompositorDetection, ReadsEveryTokenOfAColonSeparatedList) {
  // Ubuntu ships "ubuntu:GNOME"; the compositor is not always the first name.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("ubuntu:GNOME"), VDISPLAY::compositor_e::mutter);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("Hyprland:wlroots"), VDISPLAY::compositor_e::hyprland);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("X-Generic:KDE"), VDISPLAY::compositor_e::kwin);
}

TEST(CompositorDetection, AcceptsVariantsOfADesktopName) {
  // "GNOME-Classic" is still Mutter; a hyphenated variant must not read as an
  // unknown compositor and fall through to the generic wlroots path.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("GNOME-Classic:GNOME"), VDISPLAY::compositor_e::mutter);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("KDE-Plasma"), VDISPLAY::compositor_e::kwin);
}

TEST(CompositorDetection, DoesNotMatchOnASubstring) {
  // The failure the token match exists to prevent: a desktop that merely
  // contains another one's name being handed that compositor's strategy.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("GNOMEish"), VDISPLAY::compositor_e::unknown);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("NOTKDE"), VDISPLAY::compositor_e::unknown);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames(""), VDISPLAY::compositor_e::unknown);
}

TEST(CompositorDetection, LeavesUnknownCompositorsUnknown) {
  // sway and wayfire have no dedicated strategy yet: they must classify as
  // unknown so the generic wlr-output-management path stays in charge, rather
  // than being silently handed the KDE or GNOME one.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("sway"), VDISPLAY::compositor_e::unknown);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("wlroots:wayfire"), VDISPLAY::compositor_e::unknown);
}
