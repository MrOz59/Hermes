/**
 * @file virtualhid_common.h
 * @brief Shared state for the Linux libvirtualhid input backend.
 */
#pragma once

#include "src/platform/common.h"
#include "src/platform/linux/virtual_display.h"
#include "virtualhid_pen_touch.h"

#include <array>
#include <cstdint>
#include <libvirtualhid/libvirtualhid.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace platf {
  namespace kwin {
    class connection_t;
  }

  constexpr std::uint16_t VIRTUAL_INPUT_VENDOR_ID = 0xBEEF;
  constexpr std::uint16_t VIRTUAL_INPUT_PRODUCT_ID = 0xDEAD;

  static_assert(
    VIRTUAL_INPUT_VENDOR_ID == VDISPLAY::VIRTUAL_INPUT_SETTINGS_VENDOR_ID &&
      VIRTUAL_INPUT_PRODUCT_ID == VDISPLAY::VIRTUAL_INPUT_SETTINGS_PRODUCT_ID,
    "The virtual input ids and the GNOME binding ids must remain identical"
  );

  namespace virtualhid {
    std::unique_ptr<lvh::Runtime> create_runtime();

    /**
     * @brief Compute advertised protocol capabilities from backend availability and input policy.
     * @param backend Available libvirtualhid backend features.
     * @return Only capabilities enabled in Hermes and supported by the backend.
     */
    platform_caps::caps_t input_capabilities(const lvh::BackendCapabilities &backend);

    lvh::DeviceProfile with_hermes_identity(lvh::DeviceProfile profile, const std::string &session_tag, const std::string &display_name);

    void log_failure(const char *operation, const lvh::OperationStatus &result);
  }  // namespace virtualhid

  struct joypad_feedback_state {
    std::mutex mutex;
    std::optional<std::array<std::uint16_t, 2>> last_rumble;
    std::optional<std::array<std::uint16_t, 2>> last_trigger_rumble;
    std::optional<std::array<std::uint8_t, 3>> last_rgb_led;
  };

  struct joypad_state {
    std::unique_ptr<lvh::GamepadStateAdapter> adapter;
    std::array<std::optional<std::uint32_t>, 2> touch_pointer_ids {};
    std::uint8_t next_touch_id = 0;
    std::mutex mutex;
  };

  struct input_raw_t {
    explicit input_raw_t(std::string session_tag = {}, std::unique_ptr<lvh::Runtime> runtime = {});
    /** Input for a private KWin desktop, which creates no virtual devices. */
    explicit input_raw_t(std::shared_ptr<kwin::connection_t> private_kwin);
    ~input_raw_t() = default;

    std::string session_tag;
    std::shared_ptr<kwin::connection_t> kwin;
    std::unique_ptr<lvh::Runtime> runtime;
    std::unique_ptr<lvh::Mouse> mouse;
    std::unique_ptr<lvh::Keyboard> keyboard;
    std::vector<std::shared_ptr<joypad_state>> gamepads;
    std::mutex gamepads_mutex;
  };

  struct client_input_raw_t: public client_input_t {
    explicit client_input_raw_t(input_t &input, std::unique_ptr<lvh::Runtime> runtime = {}):
        global(static_cast<input_raw_t *>(input.get())),
        virtualhid_devices(global->kwin ? nullptr : std::make_unique<virtualhid::client_devices_t>(global->session_tag, std::move(runtime))) {}

    input_raw_t *global;
    std::unique_ptr<virtualhid::client_devices_t> virtualhid_devices;
    // Fingers this client has put down on a private KWin desktop, which tells
    // a down from a motion: the protocol has separate requests for each.
    std::set<std::uint32_t> kwin_touches;
  };
}  // namespace platf
