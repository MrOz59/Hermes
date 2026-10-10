/**
 * @file src/platform/linux/session_attach.h
 * @brief Let a Hermes that started before the graphical session catch up with it.
 *
 * Started at boot by a lingering user manager, Hermes has no display server in
 * its environment, and a process cannot be handed one afterwards. Streaming
 * the login screen and then the desktop needs none, but launching applications
 * into the desktop, driving its layout and sharing its clipboard all do.
 *
 * So once a session exists whose display variables Hermes does not carry, and
 * no client has been connected for a moment, Hermes re-executes itself with the
 * environment the user manager now holds - what `systemctl --user restart
 * hermes` would have given it, without a stream to interrupt.
 */
#pragma once

// standard includes
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace platf::session_attach {

  using environment_t = std::map<std::string, std::string>;

  /**
   * @brief Split "NAME=value" assignments; entries without a name are dropped.
   */
  environment_t parse_environment(const std::vector<std::string> &assignments);

  /**
   * @brief Parse an environment as the kernel keeps it: assignments separated
   *        by NUL bytes, as in /proc/self/environ.
   */
  environment_t parse_environment_block(std::string_view block);

  /**
   * @brief The environment Hermes was started with, not the one it has now.
   *
   * Hermes and its libraries adjust their own environment as they come up -
   * the tray sets QT_QPA_PLATFORM=minimal when it finds no display, which
   * before login is always. Carried into the re-exec, that one variable would
   * be inherited by kscreen-doctor and every other Qt tool Hermes runs, none of
   * which can reach Wayland through the minimal platform. What a restart of
   * the unit would be given is what the kernel still holds for this process,
   * laid under what the user manager holds now.
   */
  environment_t initial_environment();

  /**
   * @brief Whether @p session names a display server that @p own does not talk to.
   *
   * Only the variables that select a display server and a desktop are compared;
   * everything else in a session's environment is the session's business. A
   * session that publishes no display server - gamescope-session does not - is
   * never a reason to attach.
   */
  bool attach_needed(const environment_t &own, const environment_t &session);

  /**
   * @brief @p own with every variable of @p session laid over it, as
   *        "NAME=value" strings ready for execve().
   */
  std::vector<std::string> merged_environment(const environment_t &own, const environment_t &session);

  /**
   * @brief Whether a display server is listening where @p environment points.
   */
  bool display_server_reachable(const environment_t &environment);

  /**
   * @brief Forget display variables that point at a display server that is gone.
   *
   * A lingering user manager can keep the variables of a session after it
   * ends, and hand them to a Hermes it starts afterwards. Qt aborts a process
   * whose display variables point at nothing, and the tray is Qt: restarted
   * into the same environment each time, Hermes would stay down on a machine
   * that is by then waiting at its login screen. Call before anything reads
   * the environment, while Hermes is still single-threaded.
   */
  void drop_stale_display_environment();

  /**
   * @brief The environment the next restart must be given, or empty to keep
   *        the current one. Read by platf::restart()'s re-exec.
   */
  const std::vector<std::string> &restart_environment();

  /**
   * @brief Start watching for a session to attach to.
   * @return A guard that stops the watch, or nullptr when there is nothing to
   *         watch: Hermes already runs inside a login session, or the build has
   *         no libsystemd to ask.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

}  // namespace platf::session_attach
