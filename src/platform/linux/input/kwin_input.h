/**
 * @file src/platform/linux/input/kwin_input.h
 * @brief Keyboard, mouse and touch for a private KWin desktop, through its fake-input protocol.
 */
#pragma once

#include "virtualhid_common.h"

#include <cstdint>

namespace platf::kwin_input {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
  /** Whether this input drives a private KWin desktop instead of virtual devices. */
  bool active(const input_raw_t *raw);

  void move(input_raw_t *raw, int delta_x, int delta_y);
  void move_abs(input_raw_t *raw, float x, float y);
  void button(input_raw_t *raw, int button, bool release);
  void scroll(input_raw_t *raw, int high_resolution_distance, bool horizontal);
  void key(input_raw_t *raw, std::uint16_t key_code, bool release);
  void unicode(input_raw_t *raw, const char *utf8, int size);
  void touch(client_input_raw_t *raw, const touch_input_t &touch);
#else
  // Without the KWin transport no input is ever private, so these never run.
  inline bool active(const input_raw_t *) {
    return false;
  }

  inline void move(input_raw_t *, int, int) {}

  inline void move_abs(input_raw_t *, float, float) {}

  inline void button(input_raw_t *, int, bool) {}

  inline void scroll(input_raw_t *, int, bool) {}

  inline void key(input_raw_t *, std::uint16_t, bool) {}

  inline void unicode(input_raw_t *, const char *, int) {}

  inline void touch(client_input_raw_t *, const touch_input_t &) {}
#endif
}  // namespace platf::kwin_input
