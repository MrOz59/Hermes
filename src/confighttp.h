/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;
  void start();

  /**
   * @brief Aggregate "is the host ready to stream?" checks.
   * @return {ready: bool, checks: [{id, status: ok|warn|fail, message}]}.
   *         `ready` is false only when at least one check fails (warnings,
   *         such as a software-encoding fallback, do not block).
   */
  nlohmann::json hestia_preflight_json();
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
