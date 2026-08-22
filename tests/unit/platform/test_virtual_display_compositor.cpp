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

TEST(CompositorDetection, ClassifiesTheWlrootsFamilyAsOneCase) {
  // These take one strategy - plain wlr-output-management - so they are one
  // class. They used to classify as unknown, which reached the same code path
  // but made the diagnostics unable to distinguish "expected to work" from
  // "nobody knows what this is".
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("sway"), VDISPLAY::compositor_e::wlroots);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("wlroots:wayfire"), VDISPLAY::compositor_e::wlroots);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("river"), VDISPLAY::compositor_e::wlroots);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("labwc:wlroots"), VDISPLAY::compositor_e::wlroots);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("COSMIC"), VDISPLAY::compositor_e::cosmic);
}

TEST(CompositorDetection, NamingACompositorBeatsNamingItsFamily) {
  // "wlroots" is a family, and a desktop list is not ordered by specificity.
  // Hyprland ships "Hyprland:wlroots"; the reverse order must not turn the one
  // compositor with a dedicated strategy into the generic family.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("Hyprland:wlroots"), VDISPLAY::compositor_e::hyprland);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("wlroots:Hyprland"), VDISPLAY::compositor_e::hyprland);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("wlroots:sway"), VDISPLAY::compositor_e::wlroots);
  // A bare family name is still better than unknown: it says which path applies.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("wlroots"), VDISPLAY::compositor_e::wlroots);
}

TEST(CompositorDetection, LeavesGenuinelyUnknownSessionsUnknown) {
  // "unknown" has to keep meaning "Hermes cannot advise this session", so a
  // desktop nobody has classified must not be absorbed into the wlroots case.
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("Enlightenment"), VDISPLAY::compositor_e::unknown);
  EXPECT_EQ(VDISPLAY::compositorFromDesktopNames("XFCE:X-Generic"), VDISPLAY::compositor_e::unknown);
}

TEST(CompositorDetection, NamesEveryClassForDiagnostics) {
  // The name is user-facing: it is what the diagnostics endpoint and the
  // pre-stream warning print, so every enumerator needs one and none may be
  // empty.
  for (const auto compositor : {
         VDISPLAY::compositor_e::unknown,
         VDISPLAY::compositor_e::kwin,
         VDISPLAY::compositor_e::mutter,
         VDISPLAY::compositor_e::hyprland,
         VDISPLAY::compositor_e::wlroots,
         VDISPLAY::compositor_e::cosmic,
       }) {
    EXPECT_FALSE(VDISPLAY::compositorName(compositor).empty());
  }
  EXPECT_EQ(VDISPLAY::compositorName(VDISPLAY::compositor_e::hyprland), "Hyprland");
}
