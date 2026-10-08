/**
 * @file virtualhid.cpp
 * @brief Linux platform input implementation backed exclusively by libvirtualhid.
 */
#include "src/config.h"
#include "src/logging.h"
#include "kwin_input.h"
#include "virtualhid_common.h"
#include "virtualhid_gamepad.h"
#include "virtualhid_keyboard.h"
#include "virtualhid_mouse.h"

#include <utility>

using namespace std::literals;

namespace platf::virtualhid {
  platform_caps::caps_t input_capabilities(const lvh::BackendCapabilities &backend) {
    platform_caps::caps_t capabilities = 0;
    if (config::input.mouse && config::input.native_pen_touch && (backend.supports_touchscreen || backend.supports_pen_tablet)) {
      capabilities |= platform_caps::pen_touch;
    }
    if (config::input.controller && backend.supports_gamepad && gamepad::supports_controller_extensions()) {
      capabilities |= platform_caps::controller_touch;
    }
    return capabilities;
  }

  std::unique_ptr<lvh::Runtime> create_runtime() {
    lvh::RuntimeOptions options;
    options.backend = lvh::BackendKind::platform_default;
    options.log_callback = [](lvh::LogLevel level, const std::string &message) {
      if (level == lvh::LogLevel::error || level == lvh::LogLevel::warning) {
        BOOST_LOG(warning) << "[libvirtualhid] " << message;
      } else {
        BOOST_LOG(debug) << "[libvirtualhid] " << message;
      }
    };
    return lvh::Runtime::create(std::move(options));
  }

  lvh::DeviceProfile with_hermes_identity(lvh::DeviceProfile profile, const std::string &session_tag, const std::string &display_name) {
    profile.vendor_id = VIRTUAL_INPUT_VENDOR_ID;
    profile.product_id = VIRTUAL_INPUT_PRODUCT_ID;
    profile.version = 0x111;
    profile.name = display_name;
    profile.linux_phys = session_tag;
    return profile;
  }

  void log_failure(const char *operation, const lvh::OperationStatus &result) {
    if (!result.ok()) {
      BOOST_LOG(warning) << "[libvirtualhid] " << operation << ": " << result.message();
    }
  }
}  // namespace platf::virtualhid

namespace platf {
  input_raw_t::input_raw_t(std::string input_session_tag, std::unique_ptr<lvh::Runtime> input_runtime):
      session_tag(std::move(input_session_tag)),
      runtime(input_runtime ? std::move(input_runtime) : virtualhid::create_runtime()),
      gamepads(MAX_GAMEPADS) {
    if (!runtime) {
      BOOST_LOG(error) << "[libvirtualhid] Failed to create Linux input runtime";
      return;
    }

    lvh::CreateMouseOptions mouse_options;
    mouse_options.profile = virtualhid::with_hermes_identity(
      lvh::profiles::mouse(),
      session_tag,
      session_tag.empty() ? "Mouse passthrough" : "Hermes Session Mouse"
    );
    mouse_options.stable_id = session_tag + "-mouse";
    auto created_mouse = runtime->create_mouse(mouse_options);
    if (created_mouse) {
      mouse = std::move(created_mouse.mouse);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Mouse unavailable: " << created_mouse.status.message();
    }

    lvh::CreateKeyboardOptions keyboard_options;
    keyboard_options.profile = virtualhid::with_hermes_identity(
      lvh::profiles::keyboard(),
      session_tag,
      session_tag.empty() ? "Keyboard passthrough" : "Hermes Session Keyboard"
    );
    keyboard_options.stable_id = session_tag + "-keyboard";
    // Hermes owns repeat timing so the backend must only emit explicit events.
    keyboard_options.auto_repeat_interval_ms = 0;
    auto created_keyboard = runtime->create_keyboard(keyboard_options);
    if (created_keyboard) {
      keyboard = std::move(created_keyboard.keyboard);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Keyboard unavailable: " << created_keyboard.status.message();
    }
  }

  input_t input(const std::string &session_tag) {
    return {new input_raw_t(session_tag)};
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  void freeInput(void *pointer) {
    delete static_cast<input_raw_t *>(pointer);
  }

  void move_mouse(input_t &input, int delta_x, int delta_y) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::move(raw, delta_x, delta_y);
    }
    mouse::move(static_cast<input_raw_t *>(input.get()), delta_x, delta_y);
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::move_abs(raw, x, y);
    }
    mouse::move_abs(static_cast<input_raw_t *>(input.get()), touch_port, x, y);
  }

  void button_mouse(input_t &input, int button, bool release) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::button(raw, button, release);
    }
    mouse::button(static_cast<input_raw_t *>(input.get()), button, release);
  }

  void scroll(input_t &input, int high_resolution_distance) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::scroll(raw, high_resolution_distance, false);
    }
    mouse::scroll(static_cast<input_raw_t *>(input.get()), high_resolution_distance);
  }

  void hscroll(input_t &input, int high_resolution_distance) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::scroll(raw, high_resolution_distance, true);
    }
    mouse::hscroll(static_cast<input_raw_t *>(input.get()), high_resolution_distance);
  }

  void keyboard_update(input_t &input, std::uint16_t key_code, bool release, std::uint8_t flags) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::key(raw, key_code, release);
    }
    keyboard::update(static_cast<input_raw_t *>(input.get()), key_code, release, flags);
  }

  void unicode(input_t &input, char *utf8, int size) {
    if (auto *raw = static_cast<input_raw_t *>(input.get()); kwin_input::active(raw)) {
      return kwin_input::unicode(raw, utf8, size);
    }
    keyboard::unicode(static_cast<input_raw_t *>(input.get()), utf8, size);
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    if ((!config::input.mouse || !config::input.native_pen_touch) && touch.eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
      return;
    }
    auto *raw = static_cast<client_input_raw_t *>(input);
    if (raw && raw->virtualhid_devices) {
      virtualhid::touch_update(*raw->virtualhid_devices, touch_port, touch);
    }
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    if ((!config::input.mouse || !config::input.native_pen_touch) && pen.eventType != LI_TOUCH_EVENT_CANCEL_ALL) {
      return;
    }
    auto *raw = static_cast<client_input_raw_t *>(input);
    if (raw && raw->virtualhid_devices) {
      virtualhid::pen_update(*raw->virtualhid_devices, touch_port, pen);
    }
  }

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    if (kwin_input::active(static_cast<input_raw_t *>(input.get()))) {
      // A uinput gamepad would belong to every desktop of the user, not this one.
      BOOST_LOG(warning) << "Private KWin gamepad routing is not implemented; refusing a host input device";
      return -1;
    }
    return gamepad::alloc(static_cast<input_raw_t *>(input.get()), id, metadata, feedback_queue);
  }

  void free_gamepad(input_t &input, int index) {
    gamepad::free(static_cast<input_raw_t *>(input.get()), index);
  }

  void gamepad_update(input_t &input, int index, const gamepad_state_t &state) {
    gamepad::update(static_cast<input_raw_t *>(input.get()), index, state);
  }

  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    gamepad::touch(static_cast<input_raw_t *>(input.get()), touch);
  }

  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    gamepad::motion(static_cast<input_raw_t *>(input.get()), motion);
  }

  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    gamepad::battery(static_cast<input_raw_t *>(input.get()), battery);
  }

  platform_caps::caps_t get_capabilities() {
    const auto runtime = virtualhid::create_runtime();
    return runtime ? virtualhid::input_capabilities(runtime->capabilities()) : 0;
  }

  util::point_t get_mouse_loc(input_t &input) {
    return mouse::get_location(static_cast<input_raw_t *>(input.get()));
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    return gamepad::supported_gamepads(input);
  }
}  // namespace platf
