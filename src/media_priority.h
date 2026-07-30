/**
 * @file src/media_priority.h
 * @brief Explicit H2 priority classes for the GameStream compatibility path.
 */
#pragma once

#include "platform/common.h"

#include <cstdint>

namespace stream::priority {

  enum class media_priority_e : std::uint8_t {
    control = 0,
    audio = 1,
    video_reference = 2,
    video_normal = 3,
    fec = 5,
  };

  [[nodiscard]] constexpr bool precedes(
    media_priority_e left,
    media_priority_e right
  ) noexcept {
    return static_cast<std::uint8_t>(left) <
           static_cast<std::uint8_t>(right);
  }

  [[nodiscard]] constexpr media_priority_e video_frame_priority(
    bool is_idr
  ) noexcept {
    return is_idr ?
             media_priority_e::video_reference :
             media_priority_e::video_normal;
  }

  /**
   * @brief Map logical classes onto the coarser platform worker priorities.
   *
   * Sockets remain independent. Audio is critical so CPU contention cannot
   * place it behind video; its socket QoS still distinguishes it from control.
   */
  [[nodiscard]] constexpr platf::thread_priority_e worker_priority(
    media_priority_e priority
  ) noexcept {
    return precedes(priority, media_priority_e::video_reference) ?
             platf::thread_priority_e::critical :
             platf::thread_priority_e::high;
  }

}  // namespace stream::priority
