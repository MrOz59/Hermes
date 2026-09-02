/**
 * @file tests/unit/platform/test_wayland_layout.cpp
 * @brief Test the logical layout arithmetic in src/platform/linux/wayland.cpp.
 *
 * No compositor is needed: these check the numbers the capture path derives
 * from what wlr-output-management and xdg_output report, which is where a
 * scaled or rotated output used to be counted in mode pixels.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  #include <src/platform/linux/wayland.h>

TEST(WaylandLogicalSize, DividesTheModeByARoundedScale) {
  int width = 0;
  int height = 0;
  // 1.6 arrives as 410/256 = 1.6015625; unrounded, 3840 / that is 2398.
  wl::logical_size(3840, 2160, WL_OUTPUT_TRANSFORM_NORMAL, 410, width, height);
  EXPECT_EQ(width, 2400);
  EXPECT_EQ(height, 1350);
}

TEST(WaylandLogicalSize, SwapsTheAxesOfARotatedHead) {
  int width = 0;
  int height = 0;
  wl::logical_size(3840, 2160, WL_OUTPUT_TRANSFORM_90, wl_fixed_from_int(1), width, height);
  EXPECT_EQ(width, 2160);
  EXPECT_EQ(height, 3840);

  wl::logical_size(3840, 2160, WL_OUTPUT_TRANSFORM_FLIPPED_90, wl_fixed_from_int(1), width, height);
  EXPECT_EQ(width, 2160);
  EXPECT_EQ(height, 3840);
}

TEST(WaylandDesktopEnvelope, IncludesANegativeOrigin) {
  const auto envelope = wl::desktop_envelope({{-2400, 0, 2400, 1350}, {0, 0, 1920, 1080}});
  EXPECT_EQ(envelope.x, -2400);
  EXPECT_EQ(envelope.y, 0);
  EXPECT_EQ(envelope.width, 4320);
  EXPECT_EQ(envelope.height, 1350);
}

TEST(WaylandDesktopEnvelope, UsesTheLogicalSizeNotTheMode) {
  // A 3840x2160 monitor at scale 1.6 is 2400x1350 logical, so a 1920x1080
  // output beside it sits at 2400 and the envelope is 4320 wide, not 5760.
  int width = 0;
  int height = 0;
  wl::logical_size(3840, 2160, WL_OUTPUT_TRANSFORM_NORMAL, 410, width, height);
  const auto envelope = wl::desktop_envelope({{0, 0, width, height}, {2400, 0, 1920, 1080}});
  EXPECT_EQ(envelope.x, 0);
  EXPECT_EQ(envelope.y, 0);
  EXPECT_EQ(envelope.width, 4320);
  EXPECT_EQ(envelope.height, 1350);
}

TEST(WaylandInputGeometry, ScalesTheEnvelopeIntoTheStreamedOutputsPixels) {
  // A 1920x1080 monitor at scale 1 on the left, and the streamed 3840x2160
  // monitor at scale 1.6 (2400x1350 logical) at 1920,0: the envelope is
  // 4320x1350 logical, which is 6912x2160 in the streamed output's pixels,
  // and the offset 1920 becomes 3072.
  const auto envelope = wl::desktop_envelope({{0, 0, 1920, 1080}, {1920, 0, 2400, 1350}});
  const auto geometry = wl::input_geometry(envelope, {1920, 0, 2400, 1350}, 3840, 2160);
  EXPECT_EQ(geometry.offset_x, 3072);
  EXPECT_EQ(geometry.offset_y, 0);
  EXPECT_EQ(geometry.env_width, 6912);
  EXPECT_EQ(geometry.env_height, 2160);
}

TEST(WaylandInputGeometry, TakesAnUnknownLogicalSizeAsScaleOne) {
  const auto geometry = wl::input_geometry({0, 0, 3840, 1080}, {1920, 0, 0, 0}, 1920, 1080);
  EXPECT_EQ(geometry.offset_x, 1920);
  EXPECT_EQ(geometry.offset_y, 0);
  EXPECT_EQ(geometry.env_width, 3840);
  EXPECT_EQ(geometry.env_height, 1080);
}

#endif  // SUNSHINE_BUILD_WAYLAND
