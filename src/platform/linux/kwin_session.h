/**
 * @file src/platform/linux/kwin_session.h
 * @brief Explicit, generation-bound connections to a private KWin session.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace platf::kwin {
  struct endpoint_t {
    // Absolute socket paths. Neither connection may fall back to host defaults.
    std::string wayland_socket;
    std::string pipewire_socket;
    // Empty is accepted only when the private compositor has exactly one output.
    std::string output_name;
  };

  struct output_t {
    std::string name;
    int x {};
    int y {};
    int width {};
    int height {};
    int scale {1};
  };

  /**
   * Owns one accepted Wayland connection for its entire lifetime. A dead
   * connection stays dead: a replacement compositor requires a new session.
   * Calls are serialized; no global environment or default display is used.
   */
  class connection_t {
  public:
    static std::unique_ptr<connection_t> open(const endpoint_t &endpoint, bool capture, bool input, std::string &error);
    ~connection_t();
    connection_t(const connection_t &) = delete;
    connection_t &operator=(const connection_t &) = delete;

    const output_t &output() const;
    // The stream and its Wayland connection must outlive the PipeWire consumer.
    bool start_capture(bool cursor, std::uint64_t &serial, std::string &error);
    bool dispatch(std::chrono::milliseconds timeout);
    bool pointer_motion(double dx, double dy);
    bool pointer_absolute(double x, double y);
    bool button(std::uint32_t evdev_button, bool pressed);
    bool axis(bool horizontal, double value);
    bool key(std::uint32_t evdev_key, bool pressed);
    // Touch points are in the compositor's global logical coordinates. Every
    // down, motion or up is followed by its own frame.
    bool touch_down(std::uint32_t id, double x, double y);
    bool touch_motion(std::uint32_t id, double x, double y);
    bool touch_up(std::uint32_t id);
    bool keysym(std::uint32_t symbol, bool pressed);
    std::string error() const;

  private:
    struct impl_t;
    explicit connection_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::kwin
