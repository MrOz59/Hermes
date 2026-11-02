#ifdef __linux__
#include "src/platform/linux/pyrowave_fence.h"
#include <gtest/gtest.h>
#include <unistd.h>

TEST(PyroWaveFence, ConversionMustSignalBeforeEncoding) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  EXPECT_FALSE(pyrowave::wait_for_conversion_fence(fds[0], std::chrono::milliseconds(5)));
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  EXPECT_TRUE(pyrowave::wait_for_conversion_fence(fds[0], std::chrono::milliseconds(50)));
  close(fds[0]);
  close(fds[1]);
}

TEST(PyroWaveFence, BrokenOrClosedFenceCannotAuthorizeAnEncode) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  close(fds[1]);
  EXPECT_FALSE(pyrowave::wait_for_conversion_fence(fds[0], std::chrono::milliseconds(50)));
  close(fds[0]);
  EXPECT_FALSE(pyrowave::wait_for_conversion_fence(fds[0], std::chrono::milliseconds(50)));
}
#endif
