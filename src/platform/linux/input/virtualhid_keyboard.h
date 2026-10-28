/**
 * @file virtualhid_keyboard.h
 * @brief Keyboard translation for the Linux libvirtualhid backend.
 */
#pragma once

#include "virtualhid_common.h"

#include <cstdint>

namespace platf::keyboard {
  void update(input_raw_t *raw, std::uint16_t key_code, bool release, std::uint8_t flags);
  void unicode(input_raw_t *raw, const char *utf8, int size);
}  // namespace platf::keyboard
