/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <string>
#include <unordered_map>
#include <string_view>

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

  /**
   * @brief The Hestia extension capabilities document.
   *
   * Exposed so the served document can be checked against the
   * `capabilities.schema.json` this repository ships. The two had drifted -
   * the document advertised a virtual display backend the schema does not
   * allow - and every shipped Hestia rejected the whole document for it.
   */
  nlohmann::json hestia_capabilities_json();

  /**
   * @brief The fields the Web UI's config response computes for itself:
   *        host status, platform and version. Never configuration.
   *
   * Every key here must also be in config::server_computed_keys(), which is
   * what keeps a hermes.conf from overwriting one of them; a test asserts it.
   */
  nlohmann::json computed_config_json();

  /**
   * @brief Overlay a parsed hermes.conf onto @p response, in place.
   *
   * A key the response already carries is left alone. The Web UI posts the
   * whole config response back and saveConfig() persists it, so a read-only
   * field really does end up in the file - and a parsed value is always a
   * string, which replaced a JSON array with `"[]"` and took the home page
   * down with it.
   * @return @p response, for chaining.
   */
  nlohmann::json &overlay_config_file(nlohmann::json &response, const std::unordered_map<std::string, std::string> &file_values);

  /**
   * @brief Whether an address may authenticate with the Game Mode local token.
   * @details The token is capped at this machine and private/local networks even
   *          when normal Web UI logins are configured to accept WAN clients.
   */
  bool local_api_token_origin_allowed(std::string_view address);
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
