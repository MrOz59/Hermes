/**
 * @file src/platform/linux/session_bus.h
 * @brief A D-Bus session bus for an isolated session of the Hermes user.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace platf::session_bus {

  /**
   * @brief The command that starts an isolated session with a session bus of its own.
   *
   * An isolated session must not see the desktop's bus: an application that
   * registers as a unique instance would find the desktop's copy of itself
   * there and hand its window over. Hermes removes that address, and a
   * session with a uid of its own gets a bus from systemd. A session of the
   * Hermes user gets none, and what needs one fails: Steam's runtime launcher
   * exits with "Can't find session bus". `dbus-run-session` gives the session
   * a private bus that lives exactly as long as it does, as the KWin session
   * already has.
   *
   * @param command The session's command line.
   * @param has_bus Whether the session's environment already names a bus.
   * @param dbus_run_session The path of dbus-run-session, empty when it is not installed.
   * @return The command, wrapped when it needs a bus; nothing when it needs one
   *         and none can be started from a single command line.
   */
  inline std::optional<std::string> with_private_bus(std::string_view command, bool has_bus, std::string_view dbus_run_session) {
    if (has_bus) {
      return std::string {command};
    }
    if (dbus_run_session.empty() || dbus_run_session.find_first_of(" \t\n'\"\\") != std::string_view::npos) {
      return std::nullopt;
    }
    return std::string {dbus_run_session} + " -- " + std::string {command};
  }

}  // namespace platf::session_bus
