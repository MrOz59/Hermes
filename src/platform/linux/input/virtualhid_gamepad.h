/**
 * @file virtualhid_gamepad.h
 * @brief Gamepad translation for the Linux libvirtualhid backend.
 */
#pragma once

#include "virtualhid_common.h"

namespace platf::gamepad {
  int alloc(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  void free(input_raw_t *raw, int index);
  void update(input_raw_t *raw, int index, const gamepad_state_t &state);
  void touch(input_raw_t *raw, const gamepad_touch_t &touch);
  void motion(input_raw_t *raw, const gamepad_motion_t &motion);
  void battery(input_raw_t *raw, const gamepad_battery_t &battery);
  /** @brief Whether the configured controller profile can expose motion or touchpad input. */
  bool supports_controller_extensions();
  /** @brief Whether the configured Linux policy prefers an event-device gamepad. */
  bool prefer_input_events(lvh::GamepadProfileKind kind);
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);
}  // namespace platf::gamepad
