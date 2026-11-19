/**
 * @file src/platform/linux/detached_scope.h
 * @brief Run an app's detached commands in transient systemd scopes.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace platf::detached_scope {

  /**
   * The unit a detached command of one launch runs in: unique per Hermes
   * process, launch and command, and a valid unit name for any input.
   */
  inline std::string unit_name(long hermes_pid, unsigned launch, std::size_t index) {
    return "hermes-app-" + std::to_string(hermes_pid) + '-' + std::to_string(launch) + '-' + std::to_string(index) + ".scope";
  }

  /**
   * The command line that runs `command` in the transient scope `unit` of the
   * user's systemd manager. systemd-run --scope executes it in place, so its
   * pid is the one Hermes started, and everything it starts stays in the
   * scope whether it forks, calls setsid or enters a container. Stopping the
   * unit sends them all SIGTERM, then SIGKILL after `stop_timeout`.
   */
  inline std::string wrap(const std::string &systemd_run, const std::string &unit, std::chrono::seconds stop_timeout, const std::string &command) {
    return systemd_run + " --user --scope --quiet --collect --unit=" + unit + " --property=TimeoutStopSec=" +
           std::to_string(stop_timeout.count()) + " -- " + command;
  }

}  // namespace platf::detached_scope
