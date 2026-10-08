/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::touch {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch) {
#ifdef SUNSHINE_BUILD_KWIN_TRANSPORT
    if (raw->global->private_kwin) {
      auto &kwin = *raw->global->kwin_input;
      const auto &o = kwin.output();
      // touch.x/y are normalized to the streamed output, which is the private
      // desktop's only one.
      const double x = o.x + touch.x * o.width / o.scale;
      const double y = o.y + touch.y * o.height / o.scale;
      switch (touch.eventType) {
        case LI_TOUCH_EVENT_DOWN:
        case LI_TOUCH_EVENT_MOVE:
          if (raw->kwin_touches.insert(touch.pointerId).second) {
            kwin.touch_down(touch.pointerId, x, y);
          } else {
            kwin.touch_motion(touch.pointerId, x, y);
          }
          break;
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_CANCEL:
          if (raw->kwin_touches.erase(touch.pointerId)) {
            kwin.touch_up(touch.pointerId);
          }
          break;
        case LI_TOUCH_EVENT_CANCEL_ALL:
          for (const auto id : raw->kwin_touches) {
            kwin.touch_up(id);
          }
          raw->kwin_touches.clear();
          break;
        default:
          // Hover has no equivalent in the fake-input protocol.
          break;
      }
      return;
    }
#endif
    if (raw->touch) {
      switch (touch.eventType) {
        case LI_TOUCH_EVENT_HOVER:
        case LI_TOUCH_EVENT_DOWN:
        case LI_TOUCH_EVENT_MOVE:
          {
            // Convert our 0..360 range to -90..90 relative to Y axis
            int adjusted_angle = touch.rotation;

            if (adjusted_angle > 90 && adjusted_angle < 270) {
              // Lower hemisphere
              adjusted_angle = 180 - adjusted_angle;
            }

            // Wrap the value if it's out of range
            if (adjusted_angle > 90) {
              adjusted_angle -= 360;
            } else if (adjusted_angle < -90) {
              adjusted_angle += 360;
            }
            // Where the device spans the whole desktop, the streamed output's
            // offset within it is part of the coordinate; where the session
            // binds it to that output, the offset is zero.
            const float x = touch.x + static_cast<float>(touch_port.offset_x) / touch_port.width;
            const float y = touch.y + static_cast<float>(touch_port.offset_y) / touch_port.height;
            (*raw->touch).place_finger(touch.pointerId, x, y, touch.pressureOrDistance, adjusted_angle);
            break;
          }
        case LI_TOUCH_EVENT_CANCEL:
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_HOVER_LEAVE:
          {
            (*raw->touch).release_finger(touch.pointerId);
            break;
          }
          // TODO: LI_TOUCH_EVENT_CANCEL_ALL
      }
    }
  }
}  // namespace platf::touch
