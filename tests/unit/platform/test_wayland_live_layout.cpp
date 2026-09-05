/**
 * @file tests/unit/platform/test_wayland_live_layout.cpp
 * @brief Check the logical-layout arithmetic against a running compositor.
 *
 * test_wayland_layout.cpp checks the same functions against numbers written
 * down by hand, which is where the arithmetic belongs. What that cannot check
 * is whether the numbers match the ones a compositor actually lays out - and
 * that was the bug: a scale carried as 24.8 fixed point makes 1.6 into
 * 1.6015625, so 3840 divided by it rounds to 2398 where the compositor says
 * 2400, and every absolute pointer position drifted with it.
 *
 * These need a wlroots-style compositor on WAYLAND_DISPLAY and skip without
 * one. CI provides a headless sway with two outputs, one of them fractionally
 * scaled; see the `test` job in .github/workflows/build.yml.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  #include <src/platform/linux/wayland.h>

  #include <cmath>
  #include <vector>

namespace {

  /** One output as this connection sees it, with both size systems. */
  struct live_output_t {
    std::string name;
    int mode_width;
    int mode_height;
    wl::output_rect_t logical;
  };

  /**
   * The compositor's outputs, or an empty list when there is no compositor,
   * no output, or no xdg-output to describe them in logical coordinates.
   */
  std::vector<live_output_t> live_outputs(wl::display_t &display, wl::interface_t &interface) {
    std::vector<live_output_t> outputs;

    interface.listen(display.registry());
    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT] || interface.monitors.empty()) {
      return outputs;
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }
    display.roundtrip();

    for (const auto &monitor : interface.monitors) {
      // An output whose logical size never arrived says nothing about scale.
      if (monitor->logical_width <= 0 || monitor->logical_height <= 0) {
        continue;
      }
      outputs.push_back({
        monitor->name,
        monitor->viewport.width,
        monitor->viewport.height,
        {monitor->viewport.offset_x, monitor->viewport.offset_y, monitor->logical_width, monitor->logical_height},
      });
    }
    return outputs;
  }

}  // namespace

class WaylandLiveLayoutTest: public ::testing::Test {
protected:
  void SetUp() override {
    if (display.init()) {
      GTEST_SKIP() << "No Wayland compositor on WAYLAND_DISPLAY";
    }
    outputs = live_outputs(display, interface);
    if (outputs.empty()) {
      GTEST_SKIP() << "The compositor described no output in logical coordinates";
    }
  }

  wl::display_t display;
  wl::interface_t interface;
  std::vector<live_output_t> outputs;
};

TEST_F(WaylandLiveLayoutTest, LogicalSizeRecoversWhatTheCompositorLaidOut) {
  // The scale is not on the wire in a form this connection can read - wl_output
  // carries only the integer factor - so derive it from the two sizes the
  // compositor did send and check that logical_size() turns it back into the
  // compositor's own answer. On an unscaled output that is trivially true,
  // which is the point of the skip below: only a fractional scale exercises the
  // fixed-point rounding this exists to pin down.
  bool checked_a_fractional_scale = false;

  for (const auto &output : outputs) {
    if (output.mode_width <= 0 || output.mode_height <= 0) {
      continue;
    }
    const double scale = (double) output.mode_width / output.logical.width;
    if (std::lround(scale * 100.0) == 100) {
      continue;
    }
    // A rotated output swaps the axes, and this connection does not see the
    // transform; the ratio would then be nonsense. Skip rather than guess.
    if (std::lround((double) output.mode_height / output.logical.height * 100.0) != std::lround(scale * 100.0)) {
      continue;
    }
    checked_a_fractional_scale = true;

    int width = 0;
    int height = 0;
    wl::logical_size(
      output.mode_width, output.mode_height, WL_OUTPUT_TRANSFORM_NORMAL, wl_fixed_from_double(scale), width, height
    );

    EXPECT_EQ(width, output.logical.width)
      << output.name << " is " << output.mode_width << " wide at scale " << scale
      << "; the compositor lays it out " << output.logical.width << " logical pixels wide and this says " << width;
    EXPECT_EQ(height, output.logical.height) << output.name << ": logical height disagrees with the compositor";
  }

  if (!checked_a_fractional_scale) {
    GTEST_SKIP() << "Every output is at scale 1, which cannot show a rounding error. "
                    "Give one output a fractional scale to exercise this.";
  }
}

TEST_F(WaylandLiveLayoutTest, TheEnvelopeCoversEveryOutputTheCompositorHas) {
  std::vector<wl::output_rect_t> rects;
  for (const auto &output : outputs) {
    rects.push_back(output.logical);
  }

  const auto envelope = wl::desktop_envelope(rects);
  ASSERT_GT(envelope.width, 0);
  ASSERT_GT(envelope.height, 0);

  for (const auto &output : outputs) {
    EXPECT_GE(output.logical.x, envelope.x) << output.name << " starts left of the envelope";
    EXPECT_GE(output.logical.y, envelope.y) << output.name << " starts above the envelope";
    EXPECT_LE(output.logical.x + output.logical.width, envelope.x + envelope.width)
      << output.name << " runs past the right edge of the envelope";
    EXPECT_LE(output.logical.y + output.logical.height, envelope.y + envelope.height)
      << output.name << " runs past the bottom edge of the envelope";
  }
}

TEST_F(WaylandLiveLayoutTest, EveryOutputMapsInsideItsOwnCapturePixels) {
  // What the absolute-input consumer forms is (offset + x) / env, so an output's
  // offset must leave its whole width inside the envelope once both are
  // expressed in that output's capture pixels. A scaled neighbour used to be
  // counted in mode pixels, which pushed the streamed output past the edge and
  // sent every client position off the screen.
  std::vector<wl::output_rect_t> rects;
  for (const auto &output : outputs) {
    rects.push_back(output.logical);
  }
  const auto envelope = wl::desktop_envelope(rects);

  for (const auto &output : outputs) {
    if (output.mode_width <= 0 || output.mode_height <= 0) {
      continue;
    }
    const auto geometry = wl::input_geometry(envelope, output.logical, output.mode_width, output.mode_height);

    EXPECT_GE(geometry.offset_x, 0) << output.name << " has a negative offset in the envelope";
    EXPECT_GE(geometry.offset_y, 0) << output.name << " has a negative offset in the envelope";
    EXPECT_LE(geometry.offset_x + output.mode_width, geometry.env_width)
      << output.name << " is mapped past the right edge of the envelope: offset " << geometry.offset_x
      << " plus " << output.mode_width << " exceeds " << geometry.env_width;
    EXPECT_LE(geometry.offset_y + output.mode_height, geometry.env_height)
      << output.name << " is mapped past the bottom edge of the envelope";
  }
}

#endif
