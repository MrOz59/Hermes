/**
 * @file virtualhid_gamepad.cpp
 * @brief Gamepad translation for the Linux libvirtualhid backend.
 */
#include "virtualhid_gamepad.h"

#include "src/config.h"
#include "src/logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <random>
#include <string_view>
#include <utility>

using namespace std::literals;

namespace platf::gamepad {
  namespace {
    enum class controller_type {
      generic,
      xbox_360,
      xbox_one,
      xbox_series,
      dualshock4,
      dualsense,
      switch_pro,
    };

    /** @brief Configuration names and their matching Linux virtual HID profiles. */
    constexpr std::array controller_profiles {
      std::pair {"generic"sv, controller_type::generic},
      std::pair {"x360"sv, controller_type::xbox_360},
      std::pair {"xone"sv, controller_type::xbox_one},
      std::pair {"xseries"sv, controller_type::xbox_series},
      std::pair {"ds4"sv, controller_type::dualshock4},
      std::pair {"ds5"sv, controller_type::dualsense},
      std::pair {"switch"sv, controller_type::switch_pro},
    };

    bool supports_input_events(lvh::GamepadProfileKind kind) {
      using enum lvh::GamepadProfileKind;
      return kind == generic || kind == xbox_360 || kind == xbox_one || kind == xbox_series;
    }

    bool valid_index(const input_raw_t *raw, int index) {
      return raw && index >= 0 && static_cast<std::size_t>(index) < raw->gamepads.size();
    }

    std::shared_ptr<joypad_state> find_gamepad(input_raw_t *raw, int index) {
      if (!valid_index(raw, index)) {
        return {};
      }
      std::lock_guard lock {raw->gamepads_mutex};
      return raw->gamepads[index];
    }

    float normalize_stick(std::int16_t value) {
      return value < 0 ? static_cast<float>(value) / 32768.0F : static_cast<float>(value) / 32767.0F;
    }

    float normalize_trigger(std::uint8_t value) {
      return static_cast<float>(value) / 255.0F;
    }

    void set_button(lvh::ButtonSet &buttons, std::uint32_t flags, std::uint32_t flag, lvh::GamepadButton button) {
      buttons.set(button, (flags & flag) != 0);
    }

    lvh::ClientControllerType client_type(std::uint8_t type) {
      switch (type) {
        case LI_CTYPE_XBOX:
          return lvh::ClientControllerType::xbox;
        case LI_CTYPE_PS:
          return lvh::ClientControllerType::playstation;
        case LI_CTYPE_NINTENDO:
          return lvh::ClientControllerType::nintendo;
        default:
          return lvh::ClientControllerType::unknown;
      }
    }

    controller_type select_type(const gamepad_id_t &id, const gamepad_arrival_t &metadata) {
      for (const auto &[name, type] : controller_profiles) {
        if (config::input.gamepad == name) {
          BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will use " << name << " (manual selection)";
          return type;
        }
      }
      if (config::input.linux_gamepad_backend == "uinput"sv) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will use Xbox Series (automatic input-event policy)";
        return controller_type::xbox_series;
      }
      if (metadata.type == LI_CTYPE_XBOX) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox Series controller (client type)";
        return controller_type::xbox_series;
      }
      if (metadata.type == LI_CTYPE_PS) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualSense controller (client type)";
        return controller_type::dualsense;
      }
      if (metadata.type == LI_CTYPE_NINTENDO) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Nintendo Pro controller (client type)";
        return controller_type::switch_pro;
      }
      if (config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualSense controller (motion capability)";
        return controller_type::dualsense;
      }
      if (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualSense controller (touchpad capability)";
        return controller_type::dualsense;
      }

      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox Series controller (default)";
      return controller_type::xbox_series;
    }

    std::string playstation_stable_id(int global_index, std::string_view session_tag) {
      if (config::input.virtualhid_randomize_mac) {
        // A libvirtualhid Runtime numbers devices locally. Leaving stable_id
        // empty would therefore give the first DualSense in every isolated
        // Runtime the same generated MAC. Preserve the configuration's actual
        // randomisation semantics and make collisions between sessions
        // vanishingly unlikely.
        std::random_device random;
        std::array<std::uint8_t, 5> suffix {};
        for (auto &byte : suffix) {
          byte = static_cast<std::uint8_t>(random());
        }
        return std::format(
          "02:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
          suffix[0],
          suffix[1],
          suffix[2],
          suffix[3],
          suffix[4]
        );
      }

      if (session_tag.empty()) {
        return std::format("02:00:00:00:00:{:02x}", global_index & 0xFF);
      }

      // Keep deterministic MACs scoped to their private seat. The global
      // controller index alone can be reused after a session exits; including
      // the session tag prevents a recreated controller from aliasing another
      // seat's identity in hid-playstation and userspace caches.
      std::uint32_t hash = 2166136261U;
      for (const auto character : session_tag) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619U;
      }
      return std::format(
        "02:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        (hash >> 24U) & 0xFFU,
        (hash >> 16U) & 0xFFU,
        (hash >> 8U) & 0xFFU,
        hash & 0xFFU,
        global_index & 0xFF
      );
    }

    lvh::DeviceProfile profile_for(controller_type type, const std::string &session_tag) {
      lvh::DeviceProfile profile;
      switch (type) {
        case controller_type::generic:
          profile = lvh::profiles::generic_gamepad();
          profile.name = "Sunshine Generic (virtual) pad";
          break;
        case controller_type::xbox_360:
          profile = lvh::profiles::xbox_360();
          profile.name = "Sunshine X-Box 360 (virtual) pad";
          break;
        case controller_type::xbox_one:
          profile = lvh::profiles::xbox_one();
          profile.name = "Sunshine X-Box One (virtual) pad";
          break;
        case controller_type::xbox_series:
          profile = lvh::profiles::xbox_series();
          profile.name = "Sunshine X-Box Series (virtual) pad";
          break;
        case controller_type::dualshock4:
          profile = lvh::profiles::dualshock4();
          profile.name = "Sunshine PS4 (virtual) pad";
          break;
        case controller_type::dualsense:
          profile = lvh::profiles::dualsense_usb();
          profile.name = "Sunshine PS5 (virtual) pad";
          break;
        case controller_type::switch_pro:
          profile = lvh::profiles::switch_pro();
          profile.name = "Sunshine Nintendo (virtual) pad";
          break;
      }
      profile.linux_phys = session_tag;
      return profile;
    }

    lvh::CreateGamepadOptions creation_options(controller_type type, const gamepad_id_t &id, const gamepad_arrival_t &metadata, const std::string &session_tag) {
      lvh::CreateGamepadOptions options;
      options.profile = profile_for(type, session_tag);
      options.prefer_input_events = prefer_input_events(options.profile.gamepad_kind);
      options.metadata.global_index = id.globalIndex;
      options.metadata.client_relative_index = id.clientRelativeIndex;
      options.metadata.client_type = client_type(metadata.type);
      options.metadata.has_motion_sensors = (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO)) != 0;
      options.metadata.has_touchpad = (metadata.capabilities & LI_CCAP_TOUCHPAD) != 0;
      options.metadata.has_rgb_led = (metadata.capabilities & LI_CCAP_RGB_LED) != 0;
      options.metadata.has_battery = (metadata.capabilities & LI_CCAP_BATTERY_STATE) != 0;

      if (type == controller_type::dualsense || type == controller_type::dualshock4) {
        options.metadata.stable_id = playstation_stable_id(id.globalIndex, session_tag);
      } else {
        options.metadata.stable_id = session_tag.empty() ? std::format("gamepad-{}", id.globalIndex) : std::format("{}-gamepad-{}", session_tag, id.globalIndex);
      }
      return options;
    }

    void warn_unsupported_client_features(controller_type type, const lvh::GamepadProfileSupport &support, int index, const gamepad_arrival_t &metadata) {
      if (type == controller_type::dualsense || type == controller_type::dualshock4) {
        if (!(metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
          BOOST_LOG(warning) << "Gamepad " << index << " emulates PlayStation without client motion sensors";
        }
        if (!(metadata.capabilities & LI_CCAP_TOUCHPAD)) {
          BOOST_LOG(warning) << "Gamepad " << index << " emulates PlayStation without a client touchpad";
        }
        return;
      }

      if (!support.supports_motion && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(warning) << "Gamepad " << index << " motion cannot be represented by the selected profile";
      }
      if (!support.supports_touchpad && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(warning) << "Gamepad " << index << " touchpad cannot be represented by the selected profile";
      }
      if (!support.supports_rgb_led && (metadata.capabilities & LI_CCAP_RGB_LED)) {
        BOOST_LOG(warning) << "Gamepad " << index << " RGB LED cannot be represented by the selected profile";
      }
    }

    void handle_output(const std::shared_ptr<joypad_feedback_state> &state, feedback_queue_t feedback_queue, std::uint16_t client_index, const lvh::GamepadOutput &output) {
      if (!feedback_queue) {
        return;
      }

      std::lock_guard lock {state->mutex};
      switch (output.kind) {
        case lvh::GamepadOutputKind::rumble:
          {
            const std::array values {output.low_frequency_rumble, output.high_frequency_rumble};
            if (state->last_rumble == values) {
              return;
            }
            state->last_rumble = values;
            feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(client_index, values[0], values[1]));
            return;
          }
        case lvh::GamepadOutputKind::trigger_rumble:
          {
            const std::array values {output.left_trigger_rumble, output.right_trigger_rumble};
            if (state->last_trigger_rumble == values) {
              return;
            }
            state->last_trigger_rumble = values;
            feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(client_index, values[0], values[1]));
            return;
          }
        case lvh::GamepadOutputKind::rgb_led:
          {
            const std::array values {output.red, output.green, output.blue};
            if (state->last_rgb_led == values) {
              return;
            }
            state->last_rgb_led = values;
            feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(client_index, values[0], values[1], values[2]));
            return;
          }
        case lvh::GamepadOutputKind::adaptive_triggers:
          feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(client_index, output.adaptive_trigger_flags, output.left_trigger_effect_type, output.right_trigger_effect_type, output.left_trigger_effect, output.right_trigger_effect));
          return;
        case lvh::GamepadOutputKind::player_leds:
        case lvh::GamepadOutputKind::raw_report:
          return;
      }
    }

    std::optional<std::size_t> contact_slot(const joypad_state &gamepad, std::uint32_t pointer_id, bool active) {
      for (std::size_t index = 0; index < gamepad.touch_pointer_ids.size(); ++index) {
        if (gamepad.touch_pointer_ids[index] == pointer_id) {
          return index;
        }
      }
      if (!active) {
        return std::nullopt;
      }
      for (std::size_t index = 0; index < gamepad.touch_pointer_ids.size(); ++index) {
        if (!gamepad.touch_pointer_ids[index].has_value()) {
          return index;
        }
      }
      return std::nullopt;
    }
  }  // namespace

  int alloc(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    if (!valid_index(raw, id.globalIndex) || !raw->runtime) {
      return -1;
    }

    const auto type = select_type(id, metadata);
    const auto options = creation_options(type, id, metadata, raw->session_tag);
    if (options.prefer_input_events && !supports_input_events(options.profile.gamepad_kind)) {
      BOOST_LOG(warning) << "Selected gamepad profile requires native HID; choose the native or auto backend policy";
      return -1;
    }
    auto created = lvh::GamepadStateAdapter::create(
      *raw->runtime,
      options
    );
    if (!created) {
      BOOST_LOG(warning) << "Unable to create virtual gamepad " << id.globalIndex << ": "
                         << created.status.message();
      return -1;
    }

    auto state = std::make_shared<joypad_state>();
    state->adapter = std::move(created.adapter);
    warn_unsupported_client_features(type, state->adapter->support(), id.globalIndex, metadata);
    // The output worker must not own the adapter it calls back from. Keeping
    // only independent feedback state also avoids joining that worker from its
    // own callback when a controller disconnects during feedback delivery.
    const auto feedback_state = std::make_shared<joypad_feedback_state>();
    state->adapter->set_output_callback(
      [feedback_state, feedback_queue, client_index = static_cast<std::uint16_t>(id.clientRelativeIndex)](
        const lvh::GamepadOutput &output
      ) {
        handle_output(feedback_state, feedback_queue, client_index, output);
      }
    );

    if (feedback_queue && state->adapter->support().supports_motion) {
      feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
      feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
    }

    {
      std::lock_guard lock {raw->gamepads_mutex};
      raw->gamepads[id.globalIndex] = std::move(state);
    }
    return 0;
  }

  void free(input_raw_t *raw, int index) {
    if (!valid_index(raw, index)) {
      return;
    }
    std::shared_ptr<joypad_state> gamepad;
    {
      std::lock_guard lock {raw->gamepads_mutex};
      gamepad = std::exchange(raw->gamepads[index], {});
    }
    if (gamepad && gamepad->adapter) {
      std::lock_guard lock {gamepad->mutex};
      virtualhid::log_failure("gamepad close", gamepad->adapter->close());
    }
  }

  void update(input_raw_t *raw, int index, const gamepad_state_t &input) {
    if (!valid_index(raw, index)) {
      return;
    }
    const auto gamepad = find_gamepad(raw, index);
    if (!gamepad || !gamepad->adapter) {
      return;
    }

    std::lock_guard lock {gamepad->mutex};
    auto state = gamepad->adapter->state();
    state.buttons.clear();
    set_button(state.buttons, input.buttonFlags, A, lvh::GamepadButton::a);
    set_button(state.buttons, input.buttonFlags, B, lvh::GamepadButton::b);
    set_button(state.buttons, input.buttonFlags, X, lvh::GamepadButton::x);
    set_button(state.buttons, input.buttonFlags, Y, lvh::GamepadButton::y);
    set_button(state.buttons, input.buttonFlags, BACK, lvh::GamepadButton::back);
    set_button(state.buttons, input.buttonFlags, START, lvh::GamepadButton::start);
    set_button(state.buttons, input.buttonFlags, HOME, lvh::GamepadButton::guide);
    set_button(state.buttons, input.buttonFlags, LEFT_STICK, lvh::GamepadButton::left_stick);
    set_button(state.buttons, input.buttonFlags, RIGHT_STICK, lvh::GamepadButton::right_stick);
    set_button(state.buttons, input.buttonFlags, LEFT_BUTTON, lvh::GamepadButton::left_shoulder);
    set_button(state.buttons, input.buttonFlags, RIGHT_BUTTON, lvh::GamepadButton::right_shoulder);
    set_button(state.buttons, input.buttonFlags, DPAD_UP, lvh::GamepadButton::dpad_up);
    set_button(state.buttons, input.buttonFlags, DPAD_DOWN, lvh::GamepadButton::dpad_down);
    set_button(state.buttons, input.buttonFlags, DPAD_LEFT, lvh::GamepadButton::dpad_left);
    set_button(state.buttons, input.buttonFlags, DPAD_RIGHT, lvh::GamepadButton::dpad_right);
    set_button(state.buttons, input.buttonFlags, MISC_BUTTON, lvh::GamepadButton::misc1);
    set_button(state.buttons, input.buttonFlags, TOUCHPAD_BUTTON, lvh::GamepadButton::touchpad);
    if (gamepad->adapter->support().supports_touchpad && config::input.ds4_back_as_touchpad_click && (input.buttonFlags & BACK)) {
      state.buttons.set(lvh::GamepadButton::touchpad);
    }
    set_button(state.buttons, input.buttonFlags, PADDLE1, lvh::GamepadButton::paddle1);
    set_button(state.buttons, input.buttonFlags, PADDLE2, lvh::GamepadButton::paddle2);
    set_button(state.buttons, input.buttonFlags, PADDLE3, lvh::GamepadButton::paddle3);
    set_button(state.buttons, input.buttonFlags, PADDLE4, lvh::GamepadButton::paddle4);
    state.left_stick = {normalize_stick(input.lsX), normalize_stick(input.lsY)};
    state.right_stick = {normalize_stick(input.rsX), normalize_stick(input.rsY)};
    state.left_trigger = normalize_trigger(input.lt);
    state.right_trigger = normalize_trigger(input.rt);
    virtualhid::log_failure("gamepad state", gamepad->adapter->set_state(state));
  }

  void touch(input_raw_t *raw, const gamepad_touch_t &input) {
    if (!valid_index(raw, input.id.globalIndex)) {
      return;
    }
    const auto gamepad = find_gamepad(raw, input.id.globalIndex);
    if (!gamepad || !gamepad->adapter || !gamepad->adapter->support().supports_touchpad) {
      return;
    }

    std::lock_guard lock {gamepad->mutex};
    if (input.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      auto state = gamepad->adapter->state();
      for (auto &contact : state.touchpad_contacts) {
        contact.active = false;
      }
      const auto result = gamepad->adapter->set_state(state);
      virtualhid::log_failure("gamepad touch cancel all", result);
      if (result.ok()) {
        gamepad->touch_pointer_ids.fill(std::nullopt);
      }
      return;
    }
    const bool active = input.eventType == LI_TOUCH_EVENT_DOWN || input.eventType == LI_TOUCH_EVENT_MOVE;
    if (!active && input.eventType != LI_TOUCH_EVENT_UP && input.eventType != LI_TOUCH_EVENT_CANCEL && input.eventType != LI_TOUCH_EVENT_HOVER_LEAVE) {
      return;
    }
    if (active && (!std::isfinite(input.x) || !std::isfinite(input.y))) {
      return;
    }
    const auto slot = contact_slot(*gamepad, input.pointerId, active);
    if (!slot.has_value()) {
      if (active) {
        BOOST_LOG(warning) << "Dropping third simultaneous gamepad touch contact";
      }
      return;
    }

    auto state = gamepad->adapter->state();
    // The wire pointer id is 32 bits, while DualSense tracking ids are 7 bits.
    // Allocate an id on contact-down instead of truncating client ids, which
    // could otherwise make two simultaneous fingers appear identical.
    auto &contact = state.touchpad_contacts[*slot];
    const bool new_contact = active && !contact.active;
    auto tracking_id = static_cast<std::uint8_t>(gamepad->next_touch_id & 0x7F);
    if (new_contact) {
      // A finger can remain down while the other cycles through all 128 IDs.
      // Never reuse the still-active finger's ID when the counter wraps.
      for (const auto &other : state.touchpad_contacts) {
        if (other.active && other.id == tracking_id) {
          tracking_id = static_cast<std::uint8_t>((tracking_id + 1) & 0x7F);
        }
      }
    }
    if (active) {
      contact = {
        .id = new_contact ? tracking_id : contact.id,
        .active = true,
        .x = std::clamp(input.x, 0.0F, 1.0F),
        .y = std::clamp(input.y, 0.0F, 1.0F),
      };
    } else {
      // Release packets need only an ID. Their unused coordinates may be
      // absent or invalid; preserve the last position so cleanup still works.
      contact.active = false;
    }
    const auto result = gamepad->adapter->set_state(state);
    virtualhid::log_failure("gamepad touch", result);
    if (result.ok()) {
      gamepad->touch_pointer_ids[*slot] = active ? std::optional {input.pointerId} : std::nullopt;
      if (new_contact) {
        gamepad->next_touch_id = static_cast<std::uint8_t>((tracking_id + 1) & 0x7F);
      }
    }
  }

  void motion(input_raw_t *raw, const gamepad_motion_t &input) {
    if (!valid_index(raw, input.id.globalIndex)) {
      return;
    }
    const auto gamepad = find_gamepad(raw, input.id.globalIndex);
    if (!gamepad || !gamepad->adapter || !gamepad->adapter->support().supports_motion) {
      return;
    }

    std::lock_guard lock {gamepad->mutex};
    auto state = gamepad->adapter->state();
    const lvh::Vector3 vector {input.x, input.y, input.z};
    switch (input.motionType) {
      case LI_MOTION_TYPE_ACCEL:
        state.acceleration = vector;
        break;
      case LI_MOTION_TYPE_GYRO:
        state.gyroscope = vector;
        break;
      default:
        return;
    }
    virtualhid::log_failure("gamepad motion", gamepad->adapter->set_state(state));
  }

  void battery(input_raw_t *raw, const gamepad_battery_t &input) {
    if (!valid_index(raw, input.id.globalIndex) || input.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN) {
      return;
    }
    const auto gamepad = find_gamepad(raw, input.id.globalIndex);
    if (!gamepad || !gamepad->adapter || !gamepad->adapter->support().supports_battery) {
      return;
    }

    lvh::GamepadBatteryState translated;
    switch (input.state) {
      case LI_BATTERY_STATE_DISCHARGING:
        translated = lvh::GamepadBatteryState::discharging;
        break;
      case LI_BATTERY_STATE_CHARGING:
        translated = lvh::GamepadBatteryState::charging;
        break;
      case LI_BATTERY_STATE_FULL:
        translated = lvh::GamepadBatteryState::full;
        break;
      case LI_BATTERY_STATE_NOT_CHARGING:
        translated = lvh::GamepadBatteryState::charging_error;
        break;
      default:
        return;
    }

    std::lock_guard lock {gamepad->mutex};
    auto state = gamepad->adapter->state();
    state.battery = lvh::GamepadBattery {
      .state = translated,
      .percentage = static_cast<std::uint8_t>(std::min<int>(input.percentage, 100)),
    };
    virtualhid::log_failure("gamepad battery", gamepad->adapter->set_state(state));
  }

  bool prefer_input_events(lvh::GamepadProfileKind kind) {
    if (config::input.linux_gamepad_backend == "native"sv) {
      return false;
    }
    return config::input.linux_gamepad_backend == "uinput"sv || supports_input_events(kind);
  }

  bool supports_controller_extensions() {
    if (config::input.linux_gamepad_backend == "uinput"sv) {
      return false;
    }
    if (config::input.gamepad == "auto"sv) {
      return true;
    }
    for (const auto &[name, type] : controller_profiles) {
      if (config::input.gamepad == name) {
        const auto &support = lvh::gamepad_profile_support(profile_for(type, {}));
        return support.supports_touchpad || support.supports_motion;
      }
    }
    return false;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector options {
      supported_gamepad_t {"auto", true, ""},
      supported_gamepad_t {"generic", false, "Input backend has not been initialized"},
      supported_gamepad_t {"x360", false, "Input backend has not been initialized"},
      supported_gamepad_t {"xone", false, "Input backend has not been initialized"},
      supported_gamepad_t {"xseries", false, "Input backend has not been initialized"},
      supported_gamepad_t {"ds4", false, "Input backend has not been initialized"},
      supported_gamepad_t {"ds5", false, "Input backend has not been initialized"},
      supported_gamepad_t {"switch", false, "Input backend has not been initialized"},
    };
    if (!input) {
      return options;
    }

    auto *raw = static_cast<input_raw_t *>(input->get());
    if (!raw || !raw->runtime) {
      return options;
    }

    const gamepad_id_t probe_id {.globalIndex = MAX_GAMEPADS - 1, .clientRelativeIndex = 0};
    const gamepad_arrival_t probe_metadata {.type = LI_CTYPE_UNKNOWN, .capabilities = 0, .supportedButtons = 0};
    for (std::size_t index = 0; index < controller_profiles.size(); ++index) {
      const auto request = creation_options(controller_profiles[index].second, probe_id, probe_metadata, raw->session_tag);
      if (request.prefer_input_events && !supports_input_events(request.profile.gamepad_kind)) {
        options[index + 1].is_enabled = false;
        options[index + 1].reason_disabled = "This profile requires native HID; change the Linux gamepad backend policy";
        continue;
      }
      auto created = raw->runtime->create_gamepad(request);
      options[index + 1].is_enabled = static_cast<bool>(created);
      options[index + 1].reason_disabled = created ? "" : created.status.message();
      if (!created) {
        BOOST_LOG(warning) << "Gamepad " << options[index + 1].name << " is disabled: "
                           << options[index + 1].reason_disabled;
      }
    }
    options.front().is_enabled = std::ranges::any_of(options.begin() + 1, options.end(), [](const auto &option) {
      return option.is_enabled;
    });
    options.front().reason_disabled = options.front().is_enabled ? "" : "No virtual gamepad profile is available";
    return options;
  }
}  // namespace platf::gamepad
