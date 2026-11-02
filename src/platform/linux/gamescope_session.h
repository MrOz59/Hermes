/**
 * @file src/platform/linux/gamescope_session.h
 * @brief The Gamescope command line an isolated application session runs.
 *
 * Kept free of process and device state, so the parts that decide what
 * Gamescope is told - which scanout option a build understands, what Steam
 * mode changes - can be checked without a Hermes-KMS card or a Gamescope
 * binary.
 */
#pragma once

#include <cctype>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace platf::gamescope {

  /**
   * How a Gamescope build is told which DRM device to scan out on.
   *
   * Upstream derives its KMS device from the Vulkan device it renders with.
   * An isolated session renders on the real GPU and scans out on its own
   * Hermes-KMS card, which has no Vulkan driver, so it needs a build that
   * takes the two separately. Without one, Gamescope opens the primary node of
   * the GPU it renders with - the card the host's desktop is on.
   */
  enum class drm_selector_e {
    none,  ///< No option: the build scans out on the device it renders with.
    prefer_drm,  ///< --prefer-drm, ValveSoftware/gamescope#2220; also read from GAMESCOPE_PREFER_DRM.
    drm_device,  ///< --drm-device, the shorter downstream patch reported in Hermes#57.
  };

  inline std::string_view drm_selector_flag(drm_selector_e selector) {
    switch (selector) {
      case drm_selector_e::prefer_drm:
        return "--prefer-drm";
      case drm_selector_e::drm_device:
        return "--drm-device";
      case drm_selector_e::none:
        break;
    }
    return {};
  }

  /**
   * Whether help text names an option on its own rather than as the start of
   * a longer one, so "--prefer-drm" is not found inside "--prefer-drm-node".
   */
  inline bool help_lists_option(std::string_view help, std::string_view option) {
    const auto is_option_char = [](char ch) {
      return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_';
    };
    for (auto pos = help.find(option); pos != std::string_view::npos; pos = help.find(option, pos + 1)) {
      const auto end = pos + option.size();
      const bool starts_alone = pos == 0 || !is_option_char(help[pos - 1]);
      const bool ends_alone = end == help.size() || !is_option_char(help[end]);
      if (starts_alone && ends_alone) {
        return true;
      }
    }
    return false;
  }

  /**
   * The scanout option a build offers, judged from its --help output.
   *
   * Gamescope prints its usage to stderr, so the caller has to have captured
   * both streams. --prefer-drm wins when a build lists both: it is the one
   * proposed upstream, and the one that validates the device before starting.
   */
  inline std::optional<drm_selector_e> drm_selector_from_help(std::string_view help) {
    if (help_lists_option(help, "--prefer-drm")) {
      return drm_selector_e::prefer_drm;
    }
    if (help_lists_option(help, "--drm-device")) {
      return drm_selector_e::drm_device;
    }
    return std::nullopt;
  }

  /**
   * HERMES_ISOLATED_GAMESCOPE_DRM_FLAG, for a build whose --help does not
   * list the option it accepts. "none" launches without one, which is only
   * useful to reproduce what an unpatched Gamescope does.
   */
  inline std::optional<drm_selector_e> drm_selector_from_override(std::string_view value) {
    if (value == "--prefer-drm" || value == "prefer-drm") {
      return drm_selector_e::prefer_drm;
    }
    if (value == "--drm-device" || value == "drm-device") {
      return drm_selector_e::drm_device;
    }
    if (value == "none") {
      return drm_selector_e::none;
    }
    return std::nullopt;
  }

  /**
   * Whether path is a DRM primary node, /dev/dri/cardN, which is what both
   * options take. A render node cannot scan out, and anything else would be
   * split into several arguments by run_command().
   */
  inline bool is_primary_card_path(std::string_view path) {
    constexpr std::string_view prefix {"/dev/dri/card"};
    if (!path.starts_with(prefix) || path.size() == prefix.size()) {
      return false;
    }
    for (const char ch : path.substr(prefix.size())) {
      if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
        return false;
      }
    }
    return true;
  }

  /**
   * Whether an application runs in Gamescope's Steam mode.
   *
   * mode is the app's gamescope-steam-mode: "on" and "off" decide outright;
   * anything else is auto, which turns it on when the command's program is
   * steam. The program is the first whitespace-separated word, as
   * run_command() splits it.
   */
  inline bool steam_mode_for(std::string_view mode, std::string_view command) {
    if (mode == "on") {
      return true;
    }
    if (mode == "off") {
      return false;
    }
    const auto begin = command.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
      return false;
    }
    auto program = command.substr(begin, command.find_first_of(" \t", begin) - begin);
    if (const auto slash = program.rfind('/'); slash != std::string_view::npos) {
      program.remove_prefix(slash + 1);
    }
    return program == "steam";
  }

  /// The optional flags an application session gets when its app sets none.
  constexpr std::string_view default_args {"--expose-wayland --force-windows-fullscreen"};

  // Call with the same parsed argument tokens run_command() will receive.
  // A per-app option must not redirect an isolated compositor to the host's
  // KMS card/backend, or replace the negotiated mode and connector.
  inline std::optional<std::string> session_option_override(std::span<const std::string> arguments) {
    constexpr std::string_view reserved[] {
      "--backend",
      "--prefer-drm",
      "--drm-device",
      "--output-width",
      "--output-height",
      "--nested-width",
      "--nested-height",
      "--nested-refresh",
      "--prefer-output"
    };
    for (const auto &argument : arguments) {
      const std::string_view option = std::string_view {argument}.substr(0, argument.find('='));
      if (option == "--") {
        return argument;
      }
      if (option.starts_with("--")) {
        // getopt_long accepts a unique prefix as well as the full option.
        for (const auto name : reserved) {
          if (name.starts_with(option)) {
            return argument;
          }
        }
      } else if (argument.starts_with('-')) {
        // GNU short options can be clustered (-eW800). Once an option takes
        // a value the rest of that token is its value, e.g. -Ffsr, not flags.
        for (const char flag : std::string_view {argument}.substr(1)) {
          if (std::string_view {"WHwhrO"}.find(flag) != std::string_view::npos) {
            return argument;
          }
          if (std::string_view {"mSFsoRTC"}.find(flag) != std::string_view::npos) {
            break;
          }
        }
      }
    }
    return std::nullopt;
  }

  struct launch_t {
    std::string binary {"gamescope"};  ///< The executable; an absolute path for Hermes' own build.
    int width = 1920;
    int height = 1080;
    int refresh_hz = 60;
    std::string connector;  ///< The session card's connector, e.g. Virtual-2.
    drm_selector_e selector = drm_selector_e::none;
    std::string scanout_card;  ///< The session card's primary node.
    bool steam_mode = false;
    std::string args;  ///< The app's gamescope-args; empty means default_args.
    std::string command;  ///< What runs inside Gamescope, after --.
  };

  /**
   * The Gamescope command line for one isolated application session.
   *
   * It is handed to run_command(), which splits on whitespace and never
   * involves a shell, so every value Hermes places here is a number, a
   * generated connector name, a /dev/dri path or a binary path checked to
   * hold no whitespace; the app's own flags and command are split the same
   * way and quotes are not interpreted.
   *
   * Steam mode is Gamescope's -e: without it every window's app id is its X
   * id, Steam's base-layer app id never matches, and the overlay stays
   * invisible while it holds the controller. It also gets a second Xwayland,
   * as SteamOS runs it, so the overlay cannot take a game's X focus - Wine
   * games minimize when it does.
   *
   * The app's flags come after Hermes' Steam-mode flags, so Gamescope's
   * last-one-wins options (--xwayland-count) can override them. The backend,
   * the card, the mode and the connector stay Hermes'.
   */
  inline std::string launch_command(const launch_t &launch) {
    auto command = std::format(
      "{} --backend=drm -W {} -H {} -w {} -h {} -r {}",
      launch.binary.empty() ? std::string {"gamescope"} : launch.binary,
      launch.width,
      launch.height,
      launch.width,
      launch.height,
      launch.refresh_hz
    );
    if (launch.selector != drm_selector_e::none && !launch.scanout_card.empty()) {
      command += std::format(" {} {}", drm_selector_flag(launch.selector), launch.scanout_card);
    }
    command += launch.steam_mode ? " -e --xwayland-count 2" : " --xwayland-count 1";
    command += " ";
    command += launch.args.empty() ? std::string {default_args} : launch.args;
    if (!launch.connector.empty()) {
      command += " -O " + launch.connector;
    }
    command += " -- " + launch.command;
    return command;
  }

}  // namespace platf::gamescope
