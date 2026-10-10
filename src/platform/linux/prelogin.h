/**
 * @file src/platform/linux/prelogin.h
 * @brief Set up and check what it takes for Hermes to be reachable before login.
 *
 * A host that boots with no monitor and no autologin has nobody to log in at
 * it. Hermes can run from boot and stream the login screen instead, but that
 * rests on several things outside Hermes - its unit starting with the user
 * manager, that manager running at boot, a greeter that lights a new output by
 * itself, and devices it can open with no session of its own. This is the one
 * place that knows the list: `hermes --prelogin` sets up the part that is
 * Hermes' to set up, and the same checks feed the log and the Web UI.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::prelogin {

  enum class state_e {
    ready,  ///< In place.
    missing,  ///< Known to be absent; `fix` says what to do.
    unknown,  ///< Could not be determined, or not something that has been run.
    note,  ///< Worth saying, and no obstacle.
  };

  struct check_t {
    std::string id;  ///< Stable name for the Web UI: "unit", "linger", "greeter", ...
    state_e state;
    std::string title;
    std::string detail;
    std::string fix;  ///< Empty unless something is to be done.
  };

  struct report_t {
    bool enabled {false};  ///< Hermes' unit is set to start at boot.
    bool ready {false};  ///< Every requirement is known to be in place.
    std::vector<check_t> checks;
  };

  struct sddm_settings_t {
    std::string display_server;  ///< [General] DisplayServer, lower-case; empty when unset.
    std::string autologin_user;  ///< [Autologin] User; SDDM logs in by itself whenever it is set.
  };

  /** What the checks are decided from, gathered by collect_facts(). */
  struct facts_t {
    std::string user;
    std::string unit_name;  ///< "hermes.service"; empty when no packaged unit was found.
    bool unit_from_boot {false};  ///< A unit in the user's own directory that does not wait for the session.
    bool unit_generated {false};  ///< ...and it is the copy `--prelogin enable` writes.
    bool unit_enabled {false};  ///< The unit is wanted by default.target.
    std::optional<bool> linger;
    std::string display_manager;  ///< "sddm", "gdm", ...; empty when none is enabled.
    std::optional<sddm_settings_t> sddm;  ///< Unset when SDDM's configuration could not be read.
    std::optional<bool> input_without_session;  ///< /dev/uinput opens with no seat grant.
    std::string virtual_display_backend;
    std::optional<bool> hermes_kms_without_session;  ///< A Hermes-KMS node opens with no seat grant.
  };

  /**
   * @brief Turn the packaged unit into one that starts with the user manager.
   *
   * The packaged unit starts with the graphical session and stops with it. Its
   * `Wants=graphical-session.target` cannot simply be kept: pulled in at boot,
   * that target would be active with no session behind it. So every reference
   * to the graphical session goes, and the unit is installed under
   * default.target. The service section is kept as packaged, whatever the
   * packaging made of its start command; a restart policy is added if it had
   * none, since Hermes leaves with the compositor at logout and has to come
   * back for the login screen.
   *
   * @param packaged_unit The unit file's text.
   * @param source_path Where it was read from, for the header.
   */
  std::string from_boot_unit(std::string_view packaged_unit, std::string_view source_path);

  /** @brief Whether @p unit was written by from_boot_unit(). */
  bool is_generated_unit(std::string_view unit);

  /**
   * @brief Whether @p unit depends on a target that only exists once somebody
   *        has logged in, whoever wrote it.
   */
  bool unit_waits_for_session(std::string_view unit);

  /**
   * @brief Read SDDM's settings from its configuration files.
   * @param files Their contents in the order SDDM applies them: the vendor
   *        directory, /etc/sddm.conf.d, then /etc/sddm.conf, which has the
   *        last word.
   */
  sddm_settings_t sddm_settings(const std::vector<std::string> &files);

  /** @brief Decide every check from @p facts. */
  report_t assess(const facts_t &facts);

  std::string state_name(state_e state);

  /** @brief Look at this machine. */
  facts_t collect_facts();

  /** @brief assess(collect_facts()). */
  report_t report();

  /**
   * @brief Say at startup what stands between this host and being reachable
   *        before login. Silent unless Hermes is set to start at boot.
   */
  void log_report();

  /**
   * @brief `hermes --prelogin status|enable|disable`.
   *
   * Runs before logging starts and prints to the terminal: starting the log
   * rotates the log file, and this is a command to run while Hermes is up.
   *
   * @return 0 on success - for `status`, when everything is in place - 1 when
   *         something is missing or failed, 2 for a usage error.
   */
  int command(const char *name, int argc, char *argv[]);

}  // namespace platf::prelogin
