/**
 * @file src/platform/linux/input/inputtino_mouse.cpp
 * @brief Definitions for inputtino mouse input handling.
 */
// lib includes
#include <cmath>
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_mouse.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::mouse {

  void move(input_raw_t *raw, int deltaX, int deltaY) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->private_kwin) {
      const auto &o = raw->kwin_input->output();
      raw->kwin_input->pointer_motion(double(deltaX) / o.scale, double(deltaY) / o.scale);
      return;
    }
#endif
    if (raw->mouse) {
      (*raw->mouse).move(deltaX, deltaY);
    }
  }

  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->private_kwin) {
      const auto &o = raw->kwin_input->output();
      raw->kwin_input->pointer_absolute(o.x + double(x) / o.scale, o.y + double(y) / o.scale);
      return;
    }
#endif
    if (raw->mouse) {
      // x/y are in the virtual display's own pixel space; the uinput device's
      // absolute range covers the whole host desktop (touch_port.width/height
      // are the desktop envelope). Without the desktop offset of the virtual
      // display, absolute input lands on whichever monitor occupies the
      // desktop origin (the physical one) instead of the virtual display.
      (*raw->mouse).move_abs(
        static_cast<int>(std::lround(touch_port.offset_x + x)),
        static_cast<int>(std::lround(touch_port.offset_y + y)),
        touch_port.width,
        touch_port.height);
    }
  }

  void button(input_raw_t *raw, int button, bool release) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->private_kwin) {
      int code;
      switch (button) {
        case BUTTON_LEFT:
          code = BTN_LEFT;
          break;
        case BUTTON_MIDDLE:
          code = BTN_MIDDLE;
          break;
        case BUTTON_RIGHT:
          code = BTN_RIGHT;
          break;
        case BUTTON_X1:
          code = BTN_SIDE;
          break;
        case BUTTON_X2:
          code = BTN_EXTRA;
          break;
        default:
          return;
      }
      raw->kwin_input->button(code, !release);
      return;
    }
#endif
    if (raw->mouse) {
      inputtino::Mouse::MOUSE_BUTTON btn_type;
      switch (button) {
        case BUTTON_LEFT:
          btn_type = inputtino::Mouse::LEFT;
          break;
        case BUTTON_MIDDLE:
          btn_type = inputtino::Mouse::MIDDLE;
          break;
        case BUTTON_RIGHT:
          btn_type = inputtino::Mouse::RIGHT;
          break;
        case BUTTON_X1:
          btn_type = inputtino::Mouse::SIDE;
          break;
        case BUTTON_X2:
          btn_type = inputtino::Mouse::EXTRA;
          break;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: " << button;
          return;
      }
      if (release) {
        (*raw->mouse).release(btn_type);
      } else {
        (*raw->mouse).press(btn_type);
      }
    }
  }

  void scroll(input_raw_t *raw, int high_res_distance) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->private_kwin) {
      raw->kwin_input->axis(false, -double(high_res_distance) / 8.0);
      return;
    }
#endif
    if (raw->mouse) {
      (*raw->mouse).vertical_scroll(high_res_distance);
    }
  }

  void hscroll(input_raw_t *raw, int high_res_distance) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->private_kwin) {
      raw->kwin_input->axis(true, double(high_res_distance) / 8.0);
      return;
    }
#endif
    if (raw->mouse) {
      (*raw->mouse).horizontal_scroll(high_res_distance);
    }
  }

  util::point_t get_location(input_raw_t *raw) {
    if (raw->mouse) {
      // TODO: decide what to do after https://github.com/games-on-whales/inputtino/issues/6 is resolved.
      // TODO: auto x = (*raw->mouse).get_absolute_x();
      // TODO: auto y = (*raw->mouse).get_absolute_y();
      return {0, 0};
    }
    return {0, 0};
  }
}  // namespace platf::mouse
