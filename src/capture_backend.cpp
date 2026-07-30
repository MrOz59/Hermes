/**
 * @file src/capture_backend.cpp
 * @brief Legacy platform adapter for the injectable capture-backend factory.
 */

#include "capture_backend.h"

#include "platform/common.h"
#include "video.h"

namespace video::capture_backend {

  std::vector<std::string>
    legacy_platform_capture_backend_t::enumerate_displays(
      platf::mem_type_e memory_type
    ) {
    return platf::display_names(memory_type);
  }

  std::shared_ptr<platf::display_t>
    legacy_platform_capture_backend_t::create_display(
      platf::mem_type_e memory_type,
      const std::string &display_name,
      const config_t &config
    ) {
    return platf::display(memory_type, display_name, config);
  }

}  // namespace video::capture_backend
