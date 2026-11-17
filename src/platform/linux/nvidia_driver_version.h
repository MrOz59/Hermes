/**
 * @file src/platform/linux/nvidia_driver_version.h
 * @brief Which NVIDIA kernel module is loaded, for capture logs.
 *
 * NVIDIA's DMA-BUF import, mapping and teardown behaviour has changed between
 * releases, and the experimental NVENC paths depend on exactly those. A log
 * from them is only comparable with another from the same driver, so the
 * GL->CUDA importer names it. The first line of /proc/driver/nvidia/version
 * carries both the version and the flavour:
 *
 *   NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  Release Build  (...)
 *   NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.82.07  Sun Aug 17 ...
 */
#pragma once

#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace platf::nvidia {

  struct driver_version_t {
    unsigned int major {0};
    unsigned int minor {0};
    unsigned int patch {0};  ///< 0 when the release has two components.
    bool open {false};  ///< The open kernel module rather than the proprietary one.
    std::string text;  ///< The version exactly as the driver prints it.
  };

  /// "N.N" or "N.N.N", and nothing else.
  inline std::optional<driver_version_t> parse_version_token(std::string_view token) {
    unsigned int parts[3] {};
    std::size_t count = 0;
    bool digits = false;
    if (token.empty() || token.size() > 31) {
      return std::nullopt;
    }
    for (const char c : token) {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        if (parts[count] > 100000) {
          return std::nullopt;
        }
        parts[count] = parts[count] * 10 + static_cast<unsigned int>(c - '0');
        digits = true;
      } else if (c == '.' && digits && count < 2) {
        ++count;
        digits = false;
      } else {
        return std::nullopt;
      }
    }
    if (!digits || count < 1) {
      return std::nullopt;
    }
    return driver_version_t {parts[0], parts[1], parts[2], false, std::string {token}};
  }

  /**
   * Parse the first line of /proc/driver/nvidia/version. The version is the
   * first whitespace-separated token made only of dotted numbers, so neither
   * the architecture ("x86_64") nor the build host or date is mistaken for it.
   */
  inline std::optional<driver_version_t> parse_version_line(std::string_view line) {
    constexpr std::string_view prefix = "NVRM version:";
    if (line.substr(0, prefix.size()) != prefix) {
      return std::nullopt;
    }
    std::string_view rest = line.substr(prefix.size());
    while (!rest.empty() && rest.front() != '\n') {
      const auto start = rest.find_first_not_of(" \t");
      if (start == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(start);
      const auto end = rest.find_first_of(" \t\n");
      const auto token = rest.substr(0, end);
      if (auto version = parse_version_token(token)) {
        version->open = line.find("Open Kernel Module") != std::string_view::npos;
        return version;
      }
      if (end == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(end);
    }
    return std::nullopt;
  }

  /// The loaded module, or nothing when no NVIDIA module is loaded.
  inline std::optional<driver_version_t> loaded_driver_version(const char *path = "/proc/driver/nvidia/version") {
    std::ifstream file {path};
    std::string line;
    if (!file || !std::getline(file, line)) {
      return std::nullopt;
    }
    return parse_version_line(line);
  }

  /// "610.43.03 (open kernel module)" or "not loaded", for one log line.
  inline std::string describe(const std::optional<driver_version_t> &version) {
    if (!version) {
      return "not loaded (/proc/driver/nvidia/version unreadable)";
    }
    return version->text + (version->open ? " (open kernel module)" : " (proprietary kernel module)");
  }

}  // namespace platf::nvidia
