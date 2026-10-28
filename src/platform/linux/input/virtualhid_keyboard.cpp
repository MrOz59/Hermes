/**
 * @file virtualhid_keyboard.cpp
 * @brief Keyboard translation for the Linux libvirtualhid backend.
 */
#include "virtualhid_keyboard.h"

#include <string>

namespace platf::keyboard {
  void update(input_raw_t *raw, std::uint16_t key_code, bool release, std::uint8_t flags) {
    if (!raw || !raw->keyboard) {
      return;
    }

    lvh::KeyboardEvent event;
    event.key_code = key_code;
    event.pressed = !release;
    event.uses_normalized_key_code = (flags & SS_KBE_FLAG_NON_NORMALIZED) == 0;
    virtualhid::log_failure("keyboard event", raw->keyboard->submit(event));
  }

  void unicode(input_raw_t *raw, const char *utf8, int size) {
    if (!raw || !raw->keyboard || !utf8 || size <= 0) {
      return;
    }

    lvh::KeyboardTextEvent event;
    event.text.assign(utf8, static_cast<std::size_t>(size));
    virtualhid::log_failure("keyboard text", raw->keyboard->type_text(event));
  }
}  // namespace platf::keyboard
