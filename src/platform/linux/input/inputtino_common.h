/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/virtual_display.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  /**
   * Every Hermes virtual input device is created with these ids.
   *
   * GNOME binds a touchscreen or a tablet to a monitor through a settings key
   * whose path it builds from the device's vendor and product, so these are
   * named rather than repeated as literals: the display code has to derive the
   * same path, and a drift between the two would silently stop the binding from
   * reaching our devices.
   */
  constexpr uint16_t VIRTUAL_INPUT_VENDOR_ID = 0xBEEF;
  constexpr uint16_t VIRTUAL_INPUT_PRODUCT_ID = 0xDEAD;

  // GNOME derives the settings path that binds our touch and pen devices to a
  // monitor from these, and the display code cannot include this header.
  static_assert(
    VIRTUAL_INPUT_VENDOR_ID == VDISPLAY::VIRTUAL_INPUT_SETTINGS_VENDOR_ID &&
      VIRTUAL_INPUT_PRODUCT_ID == VDISPLAY::VIRTUAL_INPUT_SETTINGS_PRODUCT_ID,
    "The device ids and the ids the GNOME input binding is built from have drifted apart"
  );

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

  struct input_raw_t {
    explicit input_raw_t(std::string session_tag = {}):
        session_tag(std::move(session_tag)),
        mouse(inputtino::Mouse::create({
          .name = this->session_tag.empty() ? "Mouse passthrough" : "Hermes Session Mouse",
          .vendor_id = VIRTUAL_INPUT_VENDOR_ID,
          .product_id = VIRTUAL_INPUT_PRODUCT_ID,
          .version = 0x111,
          .device_phys = this->session_tag,
          .device_uniq = this->session_tag,
        })),
        keyboard(inputtino::Keyboard::create({
          .name = this->session_tag.empty() ? "Keyboard passthrough" : "Hermes Session Keyboard",
          .vendor_id = VIRTUAL_INPUT_VENDOR_ID,
          .product_id = VIRTUAL_INPUT_PRODUCT_ID,
          .version = 0x111,
          .device_phys = this->session_tag,
          .device_uniq = this->session_tag,
        })),
        gamepads(MAX_GAMEPADS) {
      if (!mouse) {
        BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse.getErrorMessage();
      }
      if (!keyboard) {
        BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard.getErrorMessage();
      }
    }

    ~input_raw_t() = default;

    std::string session_tag;

    // All devices are wrapped in Result because it might be that we aren't able to create them (ex: udev permission denied)
    inputtino::Result<inputtino::Mouse> mouse;
    inputtino::Result<inputtino::Keyboard> keyboard;

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;
  };

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input):
        global((input_raw_t *) input.get()),
        touch(inputtino::TouchScreen::create({
          .name = global->session_tag.empty() ? "Touch passthrough" : "Hermes Session Touch",
          .vendor_id = VIRTUAL_INPUT_VENDOR_ID,
          .product_id = VIRTUAL_INPUT_PRODUCT_ID,
          .version = 0x111,
          .device_phys = global->session_tag,
          .device_uniq = global->session_tag,
        })),
        pen(inputtino::PenTablet::create({
          .name = global->session_tag.empty() ? "Pen passthrough" : "Hermes Session Pen",
          .vendor_id = VIRTUAL_INPUT_VENDOR_ID,
          .product_id = VIRTUAL_INPUT_PRODUCT_ID,
          .version = 0x111,
          .device_phys = global->session_tag,
          .device_uniq = global->session_tag,
        })) {
      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.
    inputtino::Result<inputtino::TouchScreen> touch;
    inputtino::Result<inputtino::PenTablet> pen;
  };

  inline float deg2rad(float degree) {
    return degree * (M_PI / 180.f);
  }
}  // namespace platf
