/**
 * @file src/platform/linux/hermes_kms_capture.h
 * @brief Hermes-KMS scanout readiness shared by capture and its regression tests.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>

namespace VDISPLAY::hermes_kms {

  constexpr uint64_t status_output_enabled = 1ULL << 0;
  constexpr uint64_t status_connected = 1ULL << 1;
  constexpr uint64_t status_scanout_active = 1ULL << 2;
  constexpr uint64_t status_frame_valid = 1ULL << 3;
  constexpr uint64_t status_dmabuf_export_ready = 1ULL << 4;

  struct status_t {
    uint64_t flags;
    uint64_t frame_sequence;
    uint64_t last_update_ns;
    uint64_t last_enable_ns;
    uint64_t last_disable_ns;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t encoder_id;
    uint32_t requested_width;
    uint32_t requested_height;
    uint32_t requested_refresh_hz;
    uint32_t active_width;
    uint32_t active_height;
    uint32_t active_refresh_hz;
    uint32_t framebuffer_id;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_format;
    uint32_t framebuffer_plane_count;
    uint32_t framebuffer_pitch[4];
    uint32_t framebuffer_offset[4];
    uint32_t reserved_alignment;
    uint64_t framebuffer_modifier;
    uint64_t session_id;
    int32_t owner_pid;
    uint32_t reserved0;
    /// Descriptors bound to this output's live session besides the owner's
    /// own (uapi >= 13); reset to zero by a revocation or a new session.
    uint64_t bound_fd_count;
    uint64_t reserved[5];
  };

  /**
   * Wait for a real scanout, not merely the mode accepted at hotplug time.
   * Query returns zero or an errno; clock/sleep injection keeps the deadline
   * and delayed-compositor tests deterministic without touching a live display.
   * A zero timeout performs one query, for callers with their own polling loop.
   */
  template<class Query, class Now, class SleepUntil>
  int wait_for_scanout(int &width, int &height, std::chrono::milliseconds timeout, Query query, Now now, SleepUntil sleep_until) {
    using namespace std::chrono_literals;
    width = height = 0;
    const auto deadline = now() + timeout;
    constexpr auto ready_flags = status_output_enabled | status_connected |
                                 status_scanout_active | status_frame_valid |
                                 status_dmabuf_export_ready;
    for (;;) {
      status_t status {};
      const int error = query(status);
      if (error == 0) {
        if ((status.flags & ready_flags) == ready_flags && status.active_width && status.active_height && status.framebuffer_width && status.framebuffer_height) {
          if (status.active_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) || status.active_height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            return EOVERFLOW;
          }
          width = static_cast<int>(status.active_width);
          height = static_cast<int>(status.active_height);
          return 0;
        }
      } else if (error != EINTR && error != EAGAIN && error != ENODATA) {
        // In particular EACCES is not a compositor delay: never hide a failed
        // session binding behind a timeout or a missing-geometry diagnostic.
        return error;
      }
      if (timeout == 0ms) {
        return error ? error : EAGAIN;
      }
      const auto current = now();
      if (current >= deadline) {
        return ETIMEDOUT;
      }
      sleep_until(std::min(deadline, current + 50ms));
    }
  }

}  // namespace VDISPLAY::hermes_kms
