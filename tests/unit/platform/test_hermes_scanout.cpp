/**
 * @file tests/unit/platform/test_hermes_scanout.cpp
 * @brief Regression coverage for delayed compositor scanout (issue #41).
 */
#include "../../tests_common.h"

#include <functional>
#include <src/platform/linux/hermes_kms_capture.h>
#include <src/platform/linux/virtual_display.h>

namespace {
  using namespace std::chrono_literals;
  using namespace VDISPLAY::hermes_kms;

  status_t ready_scanout() {
    status_t status {};
    status.flags = status_output_enabled | status_connected | status_scanout_active |
                   status_frame_valid | status_dmabuf_export_ready;
    status.requested_width = 1280;
    status.requested_height = 720;
    status.active_width = status.framebuffer_width = 1280;
    status.active_height = status.framebuffer_height = 720;
    return status;
  }

  class HermesScanout: public testing::Test {
  protected:
    std::chrono::steady_clock::time_point time {};
    status_t status = ready_scanout();
    int width = 99;
    int height = 99;
    int queries = 0;
    int sleeps = 0;
    std::function<int(status_t &)> query = [this](status_t &out) {
      out = status;
      return 0;
    };

    int wait(std::chrono::milliseconds timeout = 1000ms) {
      return wait_for_scanout(
        width,
        height,
        timeout,
        [this](status_t &out) {
          ++queries;
          return query(out);
        },
        [this] {
          return time;
        },
        [this](auto deadline) {
          EXPECT_GT(deadline, time);
          ++sleeps;
          time = deadline;
        }
      );
    }
  };

  TEST_F(HermesScanout, ReadyOutputStartsImmediately) {
    EXPECT_EQ(wait(), 0);
    EXPECT_EQ(width, 1280);
    EXPECT_EQ(height, 720);
    EXPECT_EQ(queries, 1);
    EXPECT_EQ(sleeps, 0);
  }

  TEST_F(HermesScanout, WaitsForTheCompositorsFirstFramebuffer) {
    // A mode accepted by KScreen is not yet a framebuffer capture can use.
    query = [this](status_t &out) {
      out = status;
      if (time.time_since_epoch() < 350ms) {
        out.flags = status_output_enabled | status_connected | status_scanout_active;
        out.framebuffer_width = out.framebuffer_height = 0;
      }
      return 0;
    };
    EXPECT_EQ(wait(), 0);
    EXPECT_EQ(time.time_since_epoch(), 350ms);
    EXPECT_EQ(width, 1280);
    EXPECT_EQ(height, 720);
  }

  TEST_F(HermesScanout, RequestedModeAloneNeverCountsAsReady) {
    status.flags = status_output_enabled | status_connected;
    status.active_width = status.active_height = 0;
    EXPECT_EQ(wait(), ETIMEDOUT);
    EXPECT_EQ(time.time_since_epoch(), 1000ms);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);
  }

  TEST_F(HermesScanout, RequiresEveryReadinessFlag) {
    for (const auto flag : {status_output_enabled, status_connected, status_scanout_active, status_frame_valid, status_dmabuf_export_ready}) {
      status = ready_scanout();
      status.flags &= ~flag;
      EXPECT_EQ(wait(0ms), EAGAIN) << flag;
      EXPECT_EQ(width, 0);
      EXPECT_EQ(height, 0);
    }
    EXPECT_EQ(sleeps, 0);
  }

  TEST_F(HermesScanout, RequiresRealGeometryEvenWithReadyFlags) {
    for (auto member : {&status_t::active_width, &status_t::active_height, &status_t::framebuffer_width, &status_t::framebuffer_height}) {
      status = ready_scanout();
      status.*member = 0;
      EXPECT_EQ(wait(0ms), EAGAIN);
    }
  }

  TEST_F(HermesScanout, UsesTheActiveModeWhenTheCompositorRejectsTheRequestedMode) {
    status.active_width = status.framebuffer_width = 1920;
    status.active_height = status.framebuffer_height = 1080;
    EXPECT_EQ(wait(), 0);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);
    EXPECT_EQ(sleeps, 0);
  }

  TEST_F(HermesScanout, AccessDeniedAndDeviceErrorsFailImmediately) {
    for (const int error : {EACCES, ENODEV, EBADF, EIO, ENOTTY}) {
      query = [error](status_t &) {
        return error;
      };
      EXPECT_EQ(wait(), error);
      EXPECT_EQ(width, 0);
      EXPECT_EQ(height, 0);
    }
    EXPECT_EQ(sleeps, 0);
    EXPECT_EQ(queries, 5);
  }

  TEST_F(HermesScanout, TransientErrorsCanRecoverWithinTheOriginalDeadline) {
    query = [this](status_t &out) {
      if (queries == 1) {
        return EINTR;
      }
      if (queries == 2) {
        return EAGAIN;
      }
      if (queries == 3) {
        return ENODATA;
      }
      out = status;
      return 0;
    };
    EXPECT_EQ(wait(), 0);
    EXPECT_EQ(time.time_since_epoch(), 150ms);
  }

  TEST_F(HermesScanout, RepeatedInterruptionsDoNotExtendTheDeadline) {
    query = [](status_t &) {
      return EINTR;
    };
    EXPECT_EQ(wait(125ms), ETIMEDOUT);
    EXPECT_EQ(time.time_since_epoch(), 125ms);
    EXPECT_EQ(sleeps, 3);
  }

  TEST_F(HermesScanout, AccountsForTimeSpentQueryingTheDriver) {
    query = [this](status_t &) {
      time += 40ms;
      return EAGAIN;
    };
    EXPECT_EQ(wait(120ms), ETIMEDOUT);
    EXPECT_EQ(queries, 2);
    EXPECT_EQ(sleeps, 1);
  }

  TEST_F(HermesScanout, RejectsUnrepresentableDimensions) {
    status.active_width = UINT32_MAX;
    EXPECT_EQ(wait(), EOVERFLOW);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);
    EXPECT_EQ(sleeps, 0);
  }

  TEST(HermesScanoutApi, InvalidDescriptorReportsItsActualError) {
    int width = 1280;
    int height = 720;
    EXPECT_FALSE(VDISPLAY::hermesKmsCaptureSize(-1, width, height, 1000));
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);
  }
}  // namespace
