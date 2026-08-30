/**
 * @file src/platform/linux/session_broker.h
 * @brief Ask the privileged broker for a client's own Unix account, and for a
 *        session running under it.
 */
#pragma once

#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace platf::session_broker {

  /**
   * @brief The account a paired client owns.
   *
   * The name and the home are derived by the broker from a slot, never from
   * anything the client sent, so neither is a value this side chooses or needs
   * to validate. The uid is what makes the isolation real: it is the only
   * boundary on Linux the kernel enforces between one client's files and
   * another's, or the host's.
   */
  struct account_t {
    std::string user;
    uid_t uid {};
    std::string home;
  };

  /**
   * @brief Whether a broker socket is there to talk to.
   *
   * False on every host that has not installed and enabled the broker, which is
   * the normal case. A caller that finds none runs the session as the Hermes
   * user, the way it did before accounts existed - less isolated, but working.
   */
  bool available();

  /**
   * @brief The client's account, created if this is the first time it asked.
   *
   * Idempotent: a client that comes back gets the account it had, which is what
   * makes its files still be there. Fails when the Hermes user is not in the
   * broker's allow file, or when every configured slot is in use.
   */
  std::optional<account_t> ensure(const std::string &client_uuid);

  /**
   * @brief The client's account if it already has one, without creating it.
   */
  std::optional<account_t> lookup(const std::string &client_uuid);

  /**
   * @brief Start `arguments` as the client's account, and return the unit name.
   *
   * The session becomes a transient systemd unit rather than a child of Hermes:
   * it needs a uid Hermes cannot assume, and a unit is also what gives it a
   * cgroup, a lifetime and a sandbox of its own. `arguments[0]` must be an
   * absolute path - the broker resolves nothing and no shell is involved on
   * either side, so nothing here needs quoting. `environment` is a list of
   * `NAME=VALUE` assignments; `XDG_RUNTIME_DIR` is set by the broker to match
   * the account and cannot be overridden from here.
   *
   * `scope` names a second unit beside the session's own, for what runs inside
   * the session once it is up; an empty scope is the session itself. A scoped
   * unit is bound to the session's, so stopping the session stops it too - the
   * way terminating the process group did when all of this was a child of
   * Hermes.
   */
  std::optional<std::string> start(
    const std::string &client_uuid,
    const std::vector<std::string> &arguments,
    const std::vector<std::string> &environment,
    const std::string &scope = {}
  );

  /**
   * @brief The name of the Wayland socket the session's compositor created.
   *
   * Hermes cannot list the session's runtime directory once it belongs to
   * another user, so the broker lists it and returns only the name. A name is
   * all that is needed: whatever connects to the socket is launched into the
   * session rather than run by Hermes. Nothing yet is not an error - a
   * compositor that has not finished starting has not made one - so this is
   * meant to be polled.
   */
  std::optional<std::string> wayland_socket(const std::string &client_uuid);

  /**
   * @brief Whether the client's session is running right now.
   *
   * A session started this way is a systemd unit, not a child of Hermes, so
   * there is no process handle on this side to ask. systemd is the only thing
   * that knows, and the broker is how this side reaches it.
   */
  bool active(const std::string &client_uuid);

  /**
   * @brief Stop the client's session unit. Stopping one that is already gone
   *        succeeds, so this is safe to call on a teardown path that may run
   *        twice.
   */
  bool stop(const std::string &client_uuid);

}  // namespace platf::session_broker
