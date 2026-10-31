/**
 * @file virtualhid_mouse.cpp
 * @brief Mouse translation for the Linux libvirtualhid backend.
 */
#include "virtualhid_mouse.h"

#include "src/logging.h"

namespace platf::mouse {
  void move(input_raw_t *raw, int delta_x, int delta_y) {
    if (raw && raw->mouse) {
      virtualhid::log_failure("relative mouse move", raw->mouse->move_relative(delta_x, delta_y));
    }
  }

  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y) {
    if (!raw || !raw->mouse) {
      return;
    }

    virtualhid::log_failure(
      "absolute mouse move",
      raw->mouse->move_absolute(
        static_cast<float>(touch_port.offset_x) + x,
        static_cast<float>(touch_port.offset_y) + y,
        touch_port.width,
        touch_port.height
      )
    );
  }

  void button(input_raw_t *raw, int button, bool release) {
    if (!raw || !raw->mouse) {
      return;
    }

    lvh::MouseButton translated;
    switch (button) {
      case BUTTON_LEFT:
        translated = lvh::MouseButton::left;
        break;
      case BUTTON_MIDDLE:
        translated = lvh::MouseButton::middle;
        break;
      case BUTTON_RIGHT:
        translated = lvh::MouseButton::right;
        break;
      case BUTTON_X1:
        translated = lvh::MouseButton::side;
        break;
      case BUTTON_X2:
        translated = lvh::MouseButton::extra;
        break;
      default:
        BOOST_LOG(warning) << "Unknown mouse button: " << button;
        return;
    }

    virtualhid::log_failure("mouse button", raw->mouse->button(translated, !release));
  }

  void scroll(input_raw_t *raw, int high_resolution_distance) {
    if (raw && raw->mouse) {
      virtualhid::log_failure("vertical mouse scroll", raw->mouse->vertical_scroll(high_resolution_distance));
    }
  }

  void hscroll(input_raw_t *raw, int high_resolution_distance) {
    if (raw && raw->mouse) {
      virtualhid::log_failure("horizontal mouse scroll", raw->mouse->horizontal_scroll(high_resolution_distance));
    }
  }

  util::point_t get_location(input_raw_t * /*raw*/) {
    // Neither uinput nor the former Linux backend can query the
    // compositor's global cursor position.
    return {0, 0};
  }
}  // namespace platf::mouse
