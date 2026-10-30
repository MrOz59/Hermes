// SPDX-License-Identifier: GPL-3.0-only
// Mode application for a verified private KWin connection. No host fallback.
#pragma once

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace platf::kwin::detached {
  struct mode_t {
    int width;
    int height;
    int refresh_mhz;
  };

  inline void validate(mode_t requested) {
    if (requested.width < 64 || requested.width > 16384 ||
        requested.height < 64 || requested.height > 16384 ||
        requested.refresh_mhz < 1000 || requested.refresh_mhz > 1000000) {
      throw std::runtime_error("Invalid private output mode");
    }
  }

  struct advertised_mode_t {
    std::string id;
    mode_t value;
  };

  struct output_t {
    std::string name;
    std::string id;
    std::string current;
    std::vector<advertised_mode_t> modes;
  };

  inline bool matches(mode_t actual, mode_t requested) {
    // CVT pixel-clock rounding can produce 119.98 Hz for a 120 Hz request.
    return actual.width == requested.width && actual.height == requested.height &&
           std::abs(actual.refresh_mhz - requested.refresh_mhz) <= 250;
  }

  inline output_t parse_output(const std::string &text) {
    const auto data = nlohmann::json::parse(text);
    const auto &outputs = data.at("outputs");
    if (!outputs.is_array() || outputs.size() != 1) {
      throw std::runtime_error("Private KWin must expose exactly one output");
    }
    const auto &entry = outputs.at(0);
    output_t result;
    result.name = entry.at("name").get<std::string>();
    const int id = entry.at("id").get<int>();
    if (id <= 0 || !result.name.starts_with("Virtual-") || !entry.at("enabled").get<bool>()) {
      throw std::runtime_error("Refusing to configure an unexpected private output");
    }
    result.id = std::to_string(id);
    result.current = entry.at("currentModeId").get<std::string>();
    for (const auto &mode : entry.at("modes")) {
      const auto mode_id = mode.at("id").get<std::string>();
      if (mode_id.empty() || !std::all_of(mode_id.begin(), mode_id.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
          })) {
        throw std::runtime_error("Invalid private output mode identifier");
      }
      const double hz = mode.at("refreshRate").get<double>();
      if (!std::isfinite(hz) || hz < 1 || hz > 1000) {
        throw std::runtime_error("Invalid private output refresh rate");
      }
      const auto &size = mode.at("size");
      mode_t value {size.at("width").get<int>(), size.at("height").get<int>(), static_cast<int>(std::lround(hz * 1000))};
      validate(value);
      result.modes.push_back({mode_id, value});
    }
    if (std::none_of(result.modes.begin(), result.modes.end(), [&](const auto &mode) {
          return mode.id == result.current;
        })) {
      throw std::runtime_error("Private output has no advertised current mode");
    }
    return result;
  }

  template<class Run>
  mode_t configure(mode_t requested, Run run) {
    validate(requested);
    auto output = parse_output(run("-j"));
    const auto identity = output;
    auto read_back = [&]() {
      output = parse_output(run("-j"));
      if (output.name != identity.name || output.id != identity.id) {
        throw std::runtime_error("Private output changed during mode application");
      }
    };
    auto current_matches = [&]() {
      return std::any_of(output.modes.begin(), output.modes.end(), [&](const auto &mode) {
        return mode.id == output.current && matches(mode.value, requested);
      });
    };
    auto find_mode = [&]() {
      return std::find_if(output.modes.begin(), output.modes.end(), [&](const auto &mode) {
        return matches(mode.value, requested);
      });
    };
    if (!current_matches()) {
      if (find_mode() == output.modes.end()) {
        run("output." + output.id + ".addCustomMode." + std::to_string(requested.width) + "." + std::to_string(requested.height) + "." + std::to_string(requested.refresh_mhz) + ".full");
        read_back();
      }
      const auto selected = find_mode();
      if (selected == output.modes.end()) {
        throw std::runtime_error("KWin did not advertise the requested custom mode; this KWin/KScreen version may not support it");
      }
      if (!current_matches()) {
        run("output." + output.id + ".mode." + selected->id);
        read_back();
      }
      // kscreen-doctor can return zero after an apply failure. Trust the mode
      // read from KWin, never the command exit status alone.
      if (!current_matches()) {
        throw std::runtime_error("KWin did not activate the requested private output mode");
      }
    }
    return std::find_if(output.modes.begin(), output.modes.end(), [&](const auto &mode) {
             return mode.id == output.current;
           })
      ->value;
  }
}  // namespace platf::kwin::detached
