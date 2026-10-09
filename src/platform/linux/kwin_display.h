/** @file kwin_display.h
 *  @brief Hermes capture adapter for an explicitly selected private KWin.
 */
#pragma once
#include "src/platform/common.h"

namespace platf {
  std::shared_ptr<display_t> kwin_display(mem_type_e memory, const video::config_t &config);
}
