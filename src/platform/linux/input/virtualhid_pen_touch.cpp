/**
 * @file virtualhid_pen_touch.cpp
 * @brief Moonlight pen/touch -> our libvirtualhid fork, Linux uinput.
 */
#include "virtualhid_pen_touch.h"

#include "src/logging.h"
#include "src/platform/linux/virtual_display.h"
#include "virtualhid_common.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace platf::virtualhid {
  namespace {
    // Preserve the coordinate convention used by Hermes' desktop binding:
    // offsets are zero for a private output and locate the output within the
    // host desktop for a shared session.
    float position(float normalized, int offset, int span) {
      if (!std::isfinite(normalized)) {
        return 0.0f;
      }
      const float value = normalized + (span > 0 ? static_cast<float>(offset) / span : 0.0f);
      return std::clamp(value, 0.0f, 1.0f);
    }

    float analog(float value) {
      return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }

    float contact_size(float value) {
      return std::isfinite(value) && value > 0.0F ? value : 0.0F;
    }

    int orientation(std::uint16_t rotation) {
      if (rotation == LI_ROT_UNKNOWN) {
        return 0;
      }
      int degrees = static_cast<int>(rotation) % 360;
      if (degrees < 0) {
        degrees += 360;
      }
      if (degrees > 90 && degrees < 270) {
        degrees = 180 - degrees;
      }
      if (degrees > 90) {
        degrees -= 360;
      }
      return std::clamp(degrees, -90, 90);
    }

  }  // namespace

  client_devices_t::client_devices_t(const std::string &session_tag, std::unique_ptr<lvh::Runtime> input_runtime) {
    runtime = input_runtime ? std::move(input_runtime) : create_runtime();
    if (!runtime) {
      BOOST_LOG(error) << "[libvirtualhid] Failed to create Linux HID runtime";
      return;
    }

    lvh::CreateTouchscreenOptions touch_options;
    touch_options.profile = with_hermes_identity(
      lvh::profiles::touchscreen(),
      session_tag,
      session_tag.empty() ? VDISPLAY::VIRTUAL_TOUCH_DEVICE_NAME : "Hermes Session Touch"
    );
    touch_options.stable_id = session_tag + "-touch";
    auto created_touch = runtime->create_touchscreen(touch_options);
    if (created_touch) {
      touch = std::move(created_touch.touchscreen);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Touchscreen unavailable: "
                         << created_touch.status.message();
    }

    lvh::CreatePenTabletOptions pen_options;
    pen_options.profile = with_hermes_identity(
      lvh::profiles::pen_tablet(),
      session_tag,
      session_tag.empty() ? VDISPLAY::VIRTUAL_PEN_DEVICE_NAME : "Hermes Session Pen"
    );
    pen_options.stable_id = session_tag + "-pen";
    auto created_pen = runtime->create_pen_tablet(pen_options);
    if (created_pen) {
      pen = std::move(created_pen.pen_tablet);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Pen tablet unavailable: "
                         << created_pen.status.message();
    }
  }

  void touch_update(client_devices_t &devices, const touch_port_t &port, const touch_input_t &event) {
    if (!devices.touch) {
      return;
    }
    const auto id = static_cast<std::int32_t>(event.pointerId);
    switch (event.eventType) {
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        {
          lvh::TouchContact contact;
          contact.id = id;
          contact.x = position(event.x, port.offset_x, port.width);
          contact.y = position(event.y, port.offset_y, port.height);
          contact.pressure = analog(event.pressureOrDistance);
          contact.orientation = orientation(event.rotation);
          contact.touching = event.eventType != LI_TOUCH_EVENT_HOVER;
          contact.viewport = {
            .offset_x = port.offset_x,
            .offset_y = port.offset_y,
            .width = port.width,
            .height = port.height,
          };
          contact.contact_major_axis = contact_size(event.contactAreaMajor);
          contact.contact_minor_axis = contact_size(event.contactAreaMinor);
          virtualhid::log_failure("touch update", devices.touch->place_contact(contact));
          devices.active_contacts.insert(id);
          return;
        }
      case LI_TOUCH_EVENT_UP:
        virtualhid::log_failure("touch release", devices.touch->release_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_CANCEL:
        virtualhid::log_failure("touch cancel", devices.touch->cancel_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        virtualhid::log_failure("touch leave", devices.touch->leave_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_CANCEL_ALL:
        {
          // Release every contact tracked for this client in one operation.
          for (const auto active : devices.active_contacts) {
            virtualhid::log_failure("touch cancel all", devices.touch->cancel_contact(active));
          }
          devices.active_contacts.clear();
          return;
        }
      default:
        return;
    }
  }

  void pen_update(client_devices_t &devices, const touch_port_t &port, const pen_input_t &event) {
    if (!devices.pen) {
      return;
    }

    // libvirtualhid syncs every button operation into its own evdev frame.
    // Only submit transitions; forwarding all three buttons at 120 Hz would
    // generate 360 unnecessary SYN_REPORT frames per second.
    const auto current_buttons = static_cast<std::uint8_t>(event.penButtons);
    auto update_button = [&](std::uint8_t bit, lvh::PenButton button) {
      if (((current_buttons ^ devices.pen_buttons) & bit) == 0) {
        return;
      }
      const bool pressed = (current_buttons & bit) != 0;
      const auto result = devices.pen->button(button, pressed);
      virtualhid::log_failure("pen button", result);
      if (result.ok()) {
        if (pressed) {
          devices.pen_buttons |= bit;
        } else {
          devices.pen_buttons &= static_cast<std::uint8_t>(~bit);
        }
      }
    };
    update_button(LI_PEN_BUTTON_PRIMARY, lvh::PenButton::primary);
    update_button(LI_PEN_BUTTON_SECONDARY, lvh::PenButton::secondary);
    update_button(LI_PEN_BUTTON_TERTIARY, lvh::PenButton::tertiary);

    if (event.eventType == LI_TOUCH_EVENT_BUTTON_ONLY) {
      return;
    }

    lvh::PenToolState tool;
    switch (event.toolType) {
      case LI_TOOL_TYPE_PEN:
        tool.tool = lvh::PenToolType::pen;
        break;
      case LI_TOOL_TYPE_ERASER:
        tool.tool = lvh::PenToolType::eraser;
        break;
      default:
        tool.tool = lvh::PenToolType::unchanged;
        break;
    }
    tool.x = position(event.x, port.offset_x, port.width);
    tool.y = position(event.y, port.offset_y, port.height);

    switch (event.eventType) {
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        tool.transition = lvh::PointerTransition::update;
        tool.pressure = analog(event.pressureOrDistance);
        break;
      case LI_TOUCH_EVENT_HOVER:
        tool.transition = lvh::PointerTransition::update;
        tool.distance = analog(event.pressureOrDistance);
        break;
      case LI_TOUCH_EVENT_UP:
        tool.transition = lvh::PointerTransition::release;
        break;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        tool.transition = lvh::PointerTransition::cancel;
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        tool.transition = lvh::PointerTransition::leave;
        break;
      default:
        return;
    }

    if (event.rotation != LI_ROT_UNKNOWN && event.tilt != LI_TILT_UNKNOWN) {
      const float rotation_rad = static_cast<float>(event.rotation) * (std::numbers::pi_v<float> / 180.0f);
      const float tilt_rad = static_cast<float>(event.tilt) * (std::numbers::pi_v<float> / 180.0f);
      const float r = std::sin(tilt_rad);
      const float z = std::cos(tilt_rad);
      tool.tilt_x = std::atan2(-std::sin(rotation_rad) * r, z) * 180.0f / std::numbers::pi_v<float>;
      tool.tilt_y = std::atan2(std::cos(rotation_rad) * r, z) * 180.0f / std::numbers::pi_v<float>;
    }

    virtualhid::log_failure("pen tool update", devices.pen->place_tool(tool));
  }
}  // namespace platf::virtualhid
