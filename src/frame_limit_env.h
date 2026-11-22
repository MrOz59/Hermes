/**
 * @file src/frame_limit_env.h
 * @brief Environment that caps an application's frame rate at the stream's.
 */
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace proc::frame_limit {

  /**
   * MANGOHUD_CONFIG with the stream's frame rate as fps_limit.
   *
   * MangoHud reads only MANGOHUD_CONFIG when it is set, unless its first option
   * is read_cfg. With no MANGOHUD_CONFIG, start with read_cfg so the user's
   * MangoHud.conf still applies. With one, keep it as it is, read_cfg or not,
   * and replace only its fps_limit.
   *
   * @param existing The MANGOHUD_CONFIG already in the environment, or empty.
   * @param fps The stream's frame rate.
   */
  inline std::string mangohud_config(std::string_view existing, int fps) {
    std::string config;
    if (existing.empty()) {
      config = "read_cfg";
    }
    std::size_t start = 0;
    while (start <= existing.size() && !existing.empty()) {
      const auto end = existing.find(',', start);
      const auto option = existing.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (!option.empty() && !option.starts_with("fps_limit=")) {
        config += config.empty() ? "" : ",";
        config += option;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    config += config.empty() ? "" : ",";
    config += "fps_limit=" + std::to_string(fps);
    return config;
  }

  /**
   * Variables that cap an application at the stream's frame rate, with no
   * extra software required: DXVK (D3D8-11 under Proton) and VKD3D-Proton
   * (D3D12) read DXVK_FRAME_RATE and VKD3D_FRAME_RATE in the builds that keep
   * their limiter. MangoHud, where the user already enables it, reads
   * fps_limit from MANGOHUD_CONFIG; this never sets MANGOHUD, so it neither
   * turns MangoHud on nor shows its overlay.
   *
   * @param fps The stream's frame rate; 0 or less means no limit.
   * @param existing_mangohud_config The MANGOHUD_CONFIG already set, or empty.
   * @return Name and value pairs to set; empty when there is no limit.
   */
  inline std::vector<std::pair<std::string, std::string>> environment(int fps, std::string_view existing_mangohud_config) {
    if (fps <= 0) {
      return {};
    }
    const auto rate = std::to_string(fps);
    return {
      {"DXVK_FRAME_RATE", rate},
      {"VKD3D_FRAME_RATE", rate},
      {"MANGOHUD_CONFIG", mangohud_config(existing_mangohud_config, fps)},
    };
  }

}  // namespace proc::frame_limit
