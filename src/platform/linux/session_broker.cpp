/**
 * @file src/platform/linux/session_broker.cpp
 * @brief Client for the privileged Hermes session account broker.
 */
#include "session_broker.h"

#include "session_broker_transport.h"
#include "src/logging.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <sys/stat.h>

namespace platf::session_broker {

  namespace {

    constexpr const char *default_socket_path = "/run/hermes/session-broker.sock";

    bool valid_client_id(std::string_view value) {
      return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F') || ch == '-';
      });
    }

    std::string socket_path() {
      // Overridable so the suite can point at a broker of its own rather than
      // at the machine's.
      if (const char *override_path = std::getenv("HERMES_SESSION_BROKER_SOCKET")) {
        return override_path;
      }
      return default_socket_path;
    }

    /**
     * @brief Send one request and return the reply line, or nothing at all.
     *
     * One request per connection: the broker answers and closes, so there is no
     * session to keep alive, nothing to resynchronise after an error, and no
     * way for one caller to hold the socket against another. `request` is sent
     * whole, which is what lets START carry a command and an environment
     * without either being split on whitespace.
     */
    std::optional<std::string> ask(const std::string &request) {
      auto result = detail::exchange(socket_path(), request, detail::request_timeout(request));
      if (!result.reply) {
        BOOST_LOG(warning) << "[session-broker] " << result.error;
      }
      return std::move(result.reply);
    }

    std::vector<std::string> fields(const std::string &line) {
      std::vector<std::string> out;
      std::istringstream stream {line};
      std::string field;
      while (stream >> field) {
        out.emplace_back(field);
      }
      return out;
    }

    /**
     * @brief The reply's fields when it succeeded, nothing when it did not.
     *        A refusal is logged with the broker's own words, which name the
     *        reason: an unlisted user, a full pool, a damaged account map.
     */
    std::optional<std::vector<std::string>> succeeded(
      const std::optional<std::string> &reply,
      const char *request
    ) {
      if (!reply) {
        return std::nullopt;
      }
      auto parts = fields(*reply);
      if (parts.empty() || parts.front() != "OK") {
        BOOST_LOG(warning) << "[session-broker] " << request << " was refused: " << *reply;
        return std::nullopt;
      }
      parts.erase(parts.begin());
      return parts;
    }

    /**
     * @brief Parse the `<user> <uid> <home>` an ENSURE or LOOKUP answers with.
     */
    std::optional<account_t> account_from(
      const std::optional<std::string> &reply,
      const char *request
    ) {
      if (!reply) {
        return std::nullopt;
      }
      std::istringstream stream {*reply};
      std::string tag, uid_text;
      account_t account;
      if (!(stream >> tag >> account.user >> uid_text) || tag != "OK") {
        BOOST_LOG(warning) << "[session-broker] " << request << " was refused or malformed: " << *reply;
        return std::nullopt;
      }
      std::getline(stream, account.home);
      if (account.home.starts_with(' ')) {
        account.home.erase(0, 1);
      }
      const auto parsed = std::from_chars(uid_text.data(), uid_text.data() + uid_text.size(), account.uid);
      const bool name = account.user.starts_with("hermes-s") && account.user.size() >= 9 && account.user.size() <= 16 &&
                        std::ranges::all_of(std::string_view {account.user}.substr(8), [](char ch) {
                          return ch >= '0' && ch <= '9';
                        });
      if (!name || account.home.empty() || account.home.front() != '/' ||
          parsed.ec != std::errc {} || parsed.ptr != uid_text.data() + uid_text.size() ||
          account.uid == 0 || account.uid == std::numeric_limits<uid_t>::max()) {
        BOOST_LOG(error) << "[session-broker] " << request << " returned an unusable account: " << *reply;
        return std::nullopt;
      }
      return account;
    }

  }  // namespace

  bool available() {
    struct stat info {};

    return ::stat(socket_path().c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
  }

  std::optional<account_t> ensure(const std::string &client_uuid) {
    if (!valid_client_id(client_uuid)) {
      return std::nullopt;
    }
    const auto account = account_from(ask("ENSURE " + client_uuid + "\n"), "ENSURE");
    if (account) {
      BOOST_LOG(info) << "[session-broker] Client " << client_uuid << " owns account "
                      << account->user << " (uid " << account->uid << ").";
    }
    return account;
  }

  std::optional<account_t> lookup(const std::string &client_uuid) {
    if (!valid_client_id(client_uuid)) {
      return std::nullopt;
    }
    return account_from(ask("LOOKUP " + client_uuid + "\n"), "LOOKUP");
  }

  std::optional<std::string> start(
    const std::string &client_uuid,
    const std::vector<std::string> &arguments,
    const std::vector<std::string> &environment,
    const std::string &scope
  ) {
    if (!valid_client_id(client_uuid) || arguments.empty()) {
      return std::nullopt;
    }
    if (scope.size() > 16 || !std::ranges::all_of(scope, [](char ch) {
          return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        })) {
      BOOST_LOG(error) << "[session-broker] Refusing a malformed application scope.";
      return std::nullopt;
    }

    std::string request = "START " + client_uuid + "\n";
    for (const auto &argument : arguments) {
      // A newline in an argument would end the line the broker is reading and
      // turn the rest into a request of its own. Nothing Hermes builds contains
      // one, so this is a refusal rather than an escape.
      if (argument.find('\n') != std::string::npos) {
        BOOST_LOG(error) << "[session-broker] Refusing to send an argument containing a newline.";
        return std::nullopt;
      }
      request += "ARG " + argument + "\n";
    }
    for (const auto &assignment : environment) {
      if (assignment.find('\n') != std::string::npos) {
        BOOST_LOG(error) << "[session-broker] Refusing to send an environment value "
                            "containing a newline.";
        return std::nullopt;
      }
      request += "ENV " + assignment + "\n";
    }
    if (!scope.empty()) {
      request += "SCOPE " + scope + "\n";
    }
    request += "END\n";

    const auto parts = succeeded(ask(request), "START");
    if (!parts || parts->empty()) {
      return std::nullopt;
    }
    BOOST_LOG(info) << "[session-broker] Started " << parts->front() << " for client "
                    << client_uuid << '.';
    return parts->front();
  }

  std::optional<std::string> wayland_socket(const std::string &client_uuid) {
    if (!valid_client_id(client_uuid)) {
      return std::nullopt;
    }
    // Quiet on failure: this is polled while a compositor starts, and for most
    // of that time the honest answer is "not yet".
    const auto reply = ask("SOCKET " + client_uuid + "\n");
    if (!reply) {
      return std::nullopt;
    }
    const auto parts = fields(*reply);
    if (parts.size() < 2 || parts.front() != "OK") {
      return std::nullopt;
    }
    return parts[1];
  }

  std::optional<bool> query_active(const std::string &client_uuid) {
    if (!valid_client_id(client_uuid)) {
      return false;
    }
    const auto reply = ask("STATUS " + client_uuid + "\n");
    if (reply && reply->starts_with("ERR notfound ")) {
      return false;
    }
    const auto parts = succeeded(reply, "STATUS");
    if (!parts || parts->size() != 2) {
      return std::nullopt;
    }
    constexpr std::string_view prefix {"hermes-session-"}, suffix {".service"};
    const std::string_view unit {parts->back()};
    if (!unit.starts_with(prefix) || !unit.ends_with(suffix) || unit.size() <= prefix.size() + suffix.size() ||
        !std::ranges::all_of(unit.substr(prefix.size(), unit.size() - prefix.size() - suffix.size()), [](char ch) {
          return ch >= '0' && ch <= '9';
        })) {
      return std::nullopt;
    }
    const auto &state = parts->front();
    if (state == "active" || state == "activating" || state == "reloading") {
      return true;
    }
    if (state == "inactive" || state == "failed" || state == "deactivating") {
      return false;
    }
    return std::nullopt;
  }

  bool active(const std::string &client_uuid) {
    return query_active(client_uuid).value_or(false);
  }

  bool stop(const std::string &client_uuid) {
    if (!valid_client_id(client_uuid)) {
      return false;
    }
    return succeeded(ask("STOP " + client_uuid + "\n"), "STOP").has_value();
  }

}  // namespace platf::session_broker
