/**
 * @file src/platform/linux/input/kwin_input.cpp
 * @brief Keyboard and mouse for a private KWin desktop, through its fake-input protocol.
 */
#include "kwin_input.h"

#include "src/logging.h"
#include "src/platform/linux/kwin_session.h"

#include <algorithm>
#include <boost/locale.hpp>
#include <libevdev/libevdev.h>
#include <map>
#include <stdexcept>

namespace platf {
  namespace {
    // evdev keys to the Moonlight key codes (Windows virtual keys) they serve.
    // An evdev key can serve more than one, so the lookup goes by virtual key.
      const std::multimap<short, short> key_mappings = {
      {KEY_BACKSPACE, 0x08},
      {KEY_TAB, 0x09},
      {KEY_ENTER, 0x0D},
      {KEY_LEFTSHIFT, 0x10},
      {KEY_LEFTCTRL, 0x11},
      {KEY_LEFTALT, 0x12},
      {KEY_CAPSLOCK, 0x14},
      {KEY_ESC, 0x1B},
      {KEY_SPACE, 0x20},
      {KEY_PAGEUP, 0x21},
      {KEY_PAGEDOWN, 0x22},
      {KEY_END, 0x23},
      {KEY_HOME, 0x24},
      {KEY_LEFT, 0x25},
      {KEY_UP, 0x26},
      {KEY_RIGHT, 0x27},
      {KEY_DOWN, 0x28},
      {KEY_SYSRQ, 0x2C},
      {KEY_INSERT, 0x2D},
      {KEY_DELETE, 0x2E},
      {KEY_0, 0x30},
      {KEY_1, 0x31},
      {KEY_2, 0x32},
      {KEY_3, 0x33},
      {KEY_4, 0x34},
      {KEY_5, 0x35},
      {KEY_6, 0x36},
      {KEY_7, 0x37},
      {KEY_8, 0x38},
      {KEY_9, 0x39},
      {KEY_A, 0x41},
      {KEY_B, 0x42},
      {KEY_C, 0x43},
      {KEY_D, 0x44},
      {KEY_E, 0x45},
      {KEY_F, 0x46},
      {KEY_G, 0x47},
      {KEY_H, 0x48},
      {KEY_I, 0x49},
      {KEY_J, 0x4A},
      {KEY_K, 0x4B},
      {KEY_L, 0x4C},
      {KEY_M, 0x4D},
      {KEY_N, 0x4E},
      {KEY_O, 0x4F},
      {KEY_P, 0x50},
      {KEY_Q, 0x51},
      {KEY_R, 0x52},
      {KEY_S, 0x53},
      {KEY_T, 0x54},
      {KEY_U, 0x55},
      {KEY_V, 0x56},
      {KEY_W, 0x57},
      {KEY_X, 0x58},
      {KEY_Y, 0x59},
      {KEY_Z, 0x5A},
      {KEY_LEFTMETA, 0x5B},
      {KEY_RIGHTMETA, 0x5C},
      {KEY_KP0, 0x60},
      {KEY_KP1, 0x61},
      {KEY_KP2, 0x62},
      {KEY_KP3, 0x63},
      {KEY_KP4, 0x64},
      {KEY_KP5, 0x65},
      {KEY_KP6, 0x66},
      {KEY_KP7, 0x67},
      {KEY_KP8, 0x68},
      {KEY_KP9, 0x69},
      {KEY_KPASTERISK, 0x6A},
      {KEY_KPPLUS, 0x6B},
      {KEY_KPMINUS, 0x6D},
      {KEY_KPDOT, 0x6E},
      {KEY_KPSLASH, 0x6F},
      {KEY_F1, 0x70},
      {KEY_F2, 0x71},
      {KEY_F3, 0x72},
      {KEY_F4, 0x73},
      {KEY_F5, 0x74},
      {KEY_F6, 0x75},
      {KEY_F7, 0x76},
      {KEY_F8, 0x77},
      {KEY_F9, 0x78},
      {KEY_F10, 0x79},
      {KEY_F11, 0x7A},
      {KEY_F12, 0x7B},
      {KEY_NUMLOCK, 0x90},
      {KEY_SCROLLLOCK, 0x91},
      {KEY_LEFTSHIFT, 0xA0},
      {KEY_RIGHTSHIFT, 0xA1},
      {KEY_LEFTCTRL, 0xA2},
      {KEY_RIGHTCTRL, 0xA3},
      {KEY_LEFTALT, 0xA4},
      {KEY_RIGHTALT, 0xA5},
      {KEY_SEMICOLON, 0xBA},
      {KEY_EQUAL, 0xBB},
      {KEY_COMMA, 0xBC},
      {KEY_MINUS, 0xBD},
      {KEY_DOT, 0xBE},
      {KEY_SLASH, 0xBF},
      {KEY_GRAVE, 0xC0},
      {KEY_LEFTBRACE, 0xDB},
      {KEY_BACKSLASH, 0xDC},
      {KEY_RIGHTBRACE, 0xDD},
      {KEY_APOSTROPHE, 0xDE},
      {KEY_102ND, 0xE2}
    };

  }  // namespace

  input_raw_t::input_raw_t(std::shared_ptr<kwin::connection_t> private_kwin):
      kwin(std::move(private_kwin)),
      gamepads(MAX_GAMEPADS) {}

  input_t input_private_kwin(const std::string &wayland_socket) {
    std::string error;
    std::shared_ptr<kwin::connection_t> connection = kwin::connection_t::open({wayland_socket, {}, {}}, false, true, error);
    if (!connection) {
      throw std::runtime_error("Private KWin input: " + error);
    }
    return {new input_raw_t(std::move(connection))};
  }
}  // namespace platf

namespace platf::kwin_input {
  bool active(const input_raw_t *raw) {
    return raw && raw->kwin;
  }

  void move(input_raw_t *raw, int delta_x, int delta_y) {
    const auto &o = raw->kwin->output();
    raw->kwin->pointer_motion(double(delta_x) / o.scale, double(delta_y) / o.scale);
  }

  void move_abs(input_raw_t *raw, float x, float y) {
    const auto &o = raw->kwin->output();
    raw->kwin->pointer_absolute(o.x + double(x) / o.scale, o.y + double(y) / o.scale);
  }

  void button(input_raw_t *raw, int button, bool release) {
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
    raw->kwin->button(code, !release);
  }

  void scroll(input_raw_t *raw, int high_resolution_distance, bool horizontal) {
    // Moonlight scrolls up with a positive distance, Wayland axes grow downwards.
    raw->kwin->axis(horizontal, (horizontal ? 1.0 : -1.0) * double(high_resolution_distance) / 8.0);
  }

  void key(input_raw_t *raw, std::uint16_t key_code, bool release) {
    const auto key = std::find_if(key_mappings.begin(), key_mappings.end(), [key_code](const auto &entry) {
      return entry.second == key_code;
    });
    if (key != key_mappings.end()) {
      raw->kwin->key(key->first, !release);
    }
  }

  void unicode(input_raw_t *raw, const char *utf8, int size) {
    if (!utf8 || size <= 0) {
      return;
    }
    try {
      const auto text = boost::locale::conv::utf_to_utf<char32_t>(utf8, utf8 + size);
      for (char32_t character : text) {
        const std::uint32_t symbol = character <= 0xff ? character : (0x01000000u | character);
        if (!raw->kwin->keysym(symbol, true) || !raw->kwin->keysym(symbol, false)) {
          BOOST_LOG(warning) << "Private KWin Unicode input failed";
          break;
        }
      }
    } catch (const std::exception &error) {
      BOOST_LOG(warning) << "Private KWin text input: " << error.what();
    }
  }
}  // namespace platf::kwin_input
