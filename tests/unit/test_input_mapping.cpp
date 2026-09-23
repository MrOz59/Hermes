/**
 * @file tests/unit/test_input_mapping.cpp
 * @brief Test the surface a touchscreen or pen is addressed in (src/input.*).
 */
#include "../tests_common.h"

#include <src/input.h>

TEST(DirectDevicePort, BoundToAnOutputIsMeasuredAgainstThatOutput) {
  // The reported case (#50): a 2560x1600 virtual output to the right of a
  // 3440-wide ultrawide. Measured against the 6000-wide desktop, a touch in
  // the middle of the client's picture landed at 2560/6000 of the output - a
  // fifth of the way across, about three inches out on a 12" tablet.
  const auto port = input::direct_device_port(2560, 1600, 3440, 0, 6000, 1600, true);

  EXPECT_EQ(port.offset_x, 0);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.width, 2560);
  EXPECT_EQ(port.height, 1600);
  // A touch in the middle of the output is the middle of the device.
  EXPECT_FLOAT_EQ(1280.0f / port.width, 0.5f);
}

TEST(DirectDevicePort, SpanningTheDesktopKeepsTheOutputWhereItIs) {
  // X11 and wlroots leave the device across the whole layout, so the streamed
  // output's offset is part of the coordinate.
  const auto port = input::direct_device_port(2560, 1600, 3440, 120, 6000, 1600, false);

  EXPECT_EQ(port.offset_x, 3440);
  EXPECT_EQ(port.offset_y, 120);
  EXPECT_EQ(port.width, 6000);
  EXPECT_EQ(port.height, 1600);
}

TEST(DirectDevicePort, SingleOutputDesktopsAreTheSameEitherWay) {
  // Why this went unnoticed: with one monitor the two answers agree.
  const auto bound = input::direct_device_port(1920, 1080, 0, 0, 1920, 1080, true);
  const auto spanning = input::direct_device_port(1920, 1080, 0, 0, 1920, 1080, false);

  EXPECT_EQ(bound.width, spanning.width);
  EXPECT_EQ(bound.height, spanning.height);
  EXPECT_EQ(bound.offset_x, spanning.offset_x);
  EXPECT_EQ(bound.offset_y, spanning.offset_y);
}

TEST(DirectDevicePort, WithoutAnOutputSizeTheDesktopIsUsed) {
  // A display that has not reported its size yet must not divide by zero.
  const auto port = input::direct_device_port(0, 0, 100, 50, 1920, 1080, true);

  EXPECT_EQ(port.offset_x, 100);
  EXPECT_EQ(port.offset_y, 50);
  EXPECT_EQ(port.width, 1920);
  EXPECT_EQ(port.height, 1080);
}
