/**
 * @file virtualhid_mouse.h
 * @brief Mouse translation for the Linux libvirtualhid backend.
 */
#pragma once

#include "src/utility.h"
#include "virtualhid_common.h"

namespace platf::mouse {
  void move(input_raw_t *raw, int delta_x, int delta_y);
  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y);
  void button(input_raw_t *raw, int button, bool release);
  void scroll(input_raw_t *raw, int high_resolution_distance);
  void hscroll(input_raw_t *raw, int high_resolution_distance);
  util::point_t get_location(input_raw_t *raw);
}  // namespace platf::mouse
