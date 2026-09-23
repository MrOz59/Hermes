/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "crypto.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM& permission);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(
    safe::mail_t mail,
    const std::string &session_tag = {}
  );

  /**
   * @brief The rectangle a touchscreen or a pen is addressed in.
   *
   * @param output_width Width of the streamed output.
   * @param output_height Height of the streamed output.
   * @param offset_x Horizontal offset of that output within the desktop.
   * @param offset_y Vertical offset of that output within the desktop.
   * @param env_width Width of the whole desktop.
   * @param env_height Height of the whole desktop.
   * @param binds_to_output Whether the session maps such a device onto one
   *        output, which makes its coordinates relative to that output.
   */
  platf::touch_port_t direct_device_port(
    int output_width,
    int output_height,
    int offset_x,
    int offset_y,
    int env_width,
    int env_height,
    bool binds_to_output
  );

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    /**
     * The rectangle a touchscreen or a pen is addressed in. A compositor that
     * binds such a device to one output measures it against that output
     * alone; elsewhere the device spans the desktop, and the streamed
     * output's place in it is part of the coordinate.
     */
    platf::touch_port_t device_port;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
