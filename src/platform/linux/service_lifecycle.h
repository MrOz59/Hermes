/**
 * @file src/platform/linux/service_lifecycle.h
 * @brief Recognize our installed user service without trusting inherited environment variables.
 */
#pragma once
#include <string_view>

namespace platf {
  inline bool is_hermes_user_service_cgroup(std::string_view entry) {
    // Accept unified and legacy systemd cgroup entries. A terminal can inherit
    // INVOCATION_ID from another unit, so that environment variable is not proof.
    const auto first = entry.find(':');
    const auto second = first == std::string_view::npos ? first : entry.find(':', first + 1);
    if (second == std::string_view::npos) {
      return false;
    }
    const auto path = entry.substr(second + 1);
    return path.find("/user@") != std::string_view::npos && path.ends_with("/hermes.service");
  }
}  // namespace platf
