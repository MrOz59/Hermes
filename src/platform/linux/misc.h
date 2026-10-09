/**
 * @file src/platform/linux/misc.h
 * @brief Miscellaneous declarations for Linux.
 */
#pragma once

// standard includes
#include <unistd.h>
#include <vector>

// local includes
#include "src/utility.h"

KITTY_USING_MOVE_T(file_t, int, -1, {
  if (el >= 0) {
    close(el);
  }
});

enum class window_system_e {
  NONE,  ///< No window system
  X11,  ///< X11
  WAYLAND,  ///< Wayland
};

extern window_system_e window_system;

namespace platf {
  /**
   * @brief Open the render node of the best GPU to encode a stream on.
   * @details adapter_name when it is set. Otherwise every real GPU is ranked
   * by the video it can encode - AV1, then HEVC Main10, HEVC, H.264 - with
   * more video memory breaking a tie, and the best one wins; for NVENC an
   * NVIDIA GPU comes first. The capture-only Hermes-KMS node never qualifies.
   * @param nvenc Whether the stream encodes with NVENC rather than VAAPI.
   * @return An owned file descriptor, or -1.
   */
  int open_encode_render_node(bool nvenc = false);
}  // namespace platf

namespace dyn {
  typedef void (*apiproc)(void);

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn
