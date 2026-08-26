// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file tools/hermes-session-broker.cpp
 * @brief Provision one Unix account per streaming client for an unprivileged Hermes.
 *
 * An isolated session is meant to be a machine of its own: its own desktop,
 * its own configuration, its own store accounts and saves, and no way to reach
 * the host's files or another client's. On Linux the boundary that actually
 * enforces that is the one the kernel already has - a separate uid. Everything
 * softer than that is a convention a determined client can walk around.
 *
 * Hermes runs as the person whose machine it streams, so it cannot create
 * accounts. This is the small root service that can, reachable over one unix
 * socket, speaking a line protocol with four requests:
 *
 *     ENSURE <client-uuid>  -> OK <user> <uid> <home>
 *     LOOKUP <client-uuid>  -> OK <user> <uid> <home>
 *     LIST                  -> OK <client-uuid>=<user>...
 *     PURGE <client-uuid>   -> OK
 *
 * ENSURE is idempotent: a client that comes back gets the account it had, which
 * is what makes its files still be there. Nothing is ever removed implicitly -
 * a client that unpairs keeps its account and its home, because a save game
 * outliving an accidental unpair is worth more than a tidy passwd file. PURGE
 * is the only destructive request and exists for the host to ask deliberately.
 *
 * Authorization is the peer's uid from SO_PEERCRED - filled in by the kernel,
 * so a client cannot claim to be someone else - checked against an allow file
 * only root can write.
 *
 * The account name is derived here, never received. A client-supplied UUID is
 * hostile input: it is validated to hex and dashes, used only as a lookup key,
 * and never reaches a command line or a path. Names come from the first free
 * slot, so they are `hermes-s01` through the configured maximum by
 * construction and cannot be steered.
 *
 * What the account deliberately is not: it has no shell, no sudo, and is in
 * none of `wheel`, `video`, `input`, `render` or the host user's groups. Its
 * access to a GPU comes from the Hermes-KMS card's own access_uid, which is
 * per-card. It is in `hermes-session`, which the shipped polkit rule refuses
 * every action to - without that rule a session on a local seat would count as
 * "active and local" to polkit and be handed over a hundred passwordless
 * actions, including powering the host off and mounting its disks.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;

  constexpr const char *default_allow_file = "/etc/hermes/session-broker.allow";
  constexpr const char *default_state_file = "/var/lib/hermes/session-users.map";
  constexpr const char *default_home_root = "/var/lib/hermes/sessions";
  constexpr const char *default_lock_file = "/run/hermes/session-broker.lock";
  constexpr const char *session_group = "hermes-session";
  constexpr const char *nologin_shell = "/usr/bin/nologin";

  // Eight matches the driver's seat pool, which is the real ceiling on
  // concurrent sessions. It is configurable because the limit that matters is
  // the machine's, and the machine this is built for is somebody's oversized
  // one.
  constexpr int default_max_sessions = 8;
  constexpr int session_ceiling = 64;

  constexpr int SD_LISTEN_FDS_START = 3;

  struct options_t {
    std::string allow_file {default_allow_file};
    std::string state_file {default_state_file};
    std::string home_root {default_home_root};
    std::string lock_file {default_lock_file};
    int max_sessions {default_max_sessions};
  };

  std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
      return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string {text.substr(begin, end - begin + 1)};
  }

  /**
   * @brief Whether a client-supplied identifier is safe to use as a key.
   *
   * This is the only value a remote client influences, so it is checked for
   * what it must be rather than filtered for what it must not: hex digits and
   * dashes, bounded length, nothing else. It never becomes part of a path, a
   * command line or an account name - it is a lookup key and nothing more -
   * but a value that cannot survive being written to a line-oriented file and
   * read back is not worth accepting in the first place.
   */
  bool valid_client_id(std::string_view id) {
    if (id.empty() || id.size() > 64) {
      return false;
    }
    return std::ranges::all_of(id, [](unsigned char ch) {
      return std::isxdigit(ch) || ch == '-';
    });
  }

  bool valid_account_name(std::string_view name) {
    if (name.size() < 9 || name.size() > 16 || !name.starts_with("hermes-s")) {
      return false;
    }
    return std::ranges::all_of(name.substr(8), [](unsigned char ch) {
      return std::isdigit(ch);
    });
  }

  std::string account_for_slot(int slot) {
    char name[32] {};
    std::snprintf(name, sizeof(name), "hermes-s%02d", slot);
    return name;
  }

  struct mapping_t {
    std::string client_id;
    std::string account;
  };

  /**
   * The map is the broker's only memory. It is read fresh per request rather
   * than cached: the file is root-only, and re-reading means an administrator
   * editing it does not have to restart anything.
   */
  std::vector<mapping_t> read_mappings(const options_t &options) {
    std::vector<mapping_t> mappings;
    std::ifstream stream {options.state_file};
    if (!stream) {
      return mappings;
    }

    std::string line;
    while (std::getline(stream, line)) {
      const auto comment = line.find('#');
      const auto entry = trim(comment == std::string::npos ? line : line.substr(0, comment));
      if (entry.empty()) {
        continue;
      }
      const auto split = entry.find(' ');
      if (split == std::string::npos) {
        continue;
      }
      mapping_t mapping {trim(entry.substr(0, split)), trim(entry.substr(split + 1))};
      // A line that does not survive validation is dropped rather than
      // repaired: the file is root's, so a bad line is damage, not input.
      if (valid_client_id(mapping.client_id) && valid_account_name(mapping.account)) {
        mappings.emplace_back(std::move(mapping));
      }
    }
    return mappings;
  }

  bool write_mappings(const options_t &options, const std::vector<mapping_t> &mappings) {
    std::error_code ec;
    fs::create_directories(fs::path {options.state_file}.parent_path(), ec);

    // Written through a temporary and renamed, so a crash cannot leave the
    // broker with a half-written idea of who owns which account.
    const auto temporary = options.state_file + ".new";
    {
      std::ofstream stream {temporary, std::ios::trunc};
      if (!stream) {
        return false;
      }
      stream << "# Managed by hermes-session-broker. <client-id> <account>\n";
      for (const auto &mapping : mappings) {
        stream << mapping.client_id << ' ' << mapping.account << '\n';
      }
      if (!stream) {
        return false;
      }
    }
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
      ::unlink(temporary.c_str());
      return false;
    }
    fs::rename(temporary, options.state_file, ec);
    if (ec) {
      ::unlink(temporary.c_str());
      return false;
    }
    return true;
  }

  /**
   * @brief Run a command with a fixed argument vector.
   *
   * No shell is involved anywhere in this broker. Every argument is either a
   * literal or a value this process derived itself, so there is nothing to
   * quote and nothing a quote could escape from.
   */
  bool run(const std::vector<std::string> &argv) {
    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &argument : argv) {
      raw.push_back(const_cast<char *>(argument.c_str()));
    }
    raw.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
      return false;
    }
    if (child == 0) {
      ::execv(raw[0], raw.data());
      ::_exit(127);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  /**
   * @brief First tool in `candidates` that exists, or empty.
   *
   * Distributions disagree about /usr/bin versus /usr/sbin for these, and the
   * broker refuses to guess: a wrong path here would fail at account creation
   * with a confusing error rather than at startup with a clear one.
   */
  std::string locate(std::initializer_list<const char *> candidates) {
    for (const auto *candidate : candidates) {
      std::error_code ec;
      if (fs::exists(candidate, ec)) {
        return candidate;
      }
    }
    return {};
  }

  /**
   * @brief Create the account for `name`, or report why not.
   *
   * useradd is asked rather than /etc/passwd written directly, because it owns
   * the locking that keeps concurrent account changes from corrupting the
   * shadow files, and getting that wrong is a class of bug worth not owning.
   * Every argument is a literal or a name this process built.
   */
  std::optional<std::string> provision(const options_t &options, const std::string &name) {
    const auto useradd = locate({"/usr/sbin/useradd", "/usr/bin/useradd"});
    if (useradd.empty()) {
      return "useradd was not found";
    }
    const auto shell = locate({nologin_shell, "/usr/sbin/nologin", "/bin/false"});
    if (shell.empty()) {
      return "no nologin shell was found";
    }
    if (::getgrnam(session_group) == nullptr) {
      return std::string {"the "} + session_group +
             " group does not exist; install the sysusers file that creates it";
    }

    const auto home = fs::path {options.home_root} / name;
    std::error_code ec;
    fs::create_directories(options.home_root, ec);
    if (ec) {
      return "the session home root could not be created";
    }
    // The root is traversable but not listable: one session cannot enumerate
    // the others, and each home below it is the account's own 0700.
    ::chmod(options.home_root.c_str(), S_IRWXU | S_IXGRP | S_IXOTH);

    if (!run({useradd,
              "--create-home",
              "--home-dir", home.string(),
              "--shell", shell,
              "--groups", session_group,
              "--comment", "Hermes isolated session",
              name})) {
      return "useradd failed";
    }

    const auto *account = ::getpwnam(name.c_str());
    if (account == nullptr) {
      return "the account was created but cannot be read back";
    }
    // useradd honours /etc/login.defs for the home mode, which is commonly
    // group- or world-readable. These homes hold another person's saves and
    // credentials, so the mode is set here rather than inherited.
    if (::chmod(home.c_str(), S_IRWXU) != 0) {
      return "the session home could not be made private";
    }
    return std::nullopt;
  }

  std::string reply_for(const options_t &options, const std::string &account) {
    const auto *entry = ::getpwnam(account.c_str());
    if (entry == nullptr) {
      return "ERR state the mapped account no longer exists";
    }
    return "OK " + account + ' ' + std::to_string(static_cast<unsigned>(entry->pw_uid)) + ' ' +
           (fs::path {options.home_root} / account).string();
  }

  std::string handle_ensure(const options_t &options, const std::string &client_id) {
    auto mappings = read_mappings(options);
    const auto existing = std::ranges::find_if(mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing != mappings.end()) {
      return reply_for(options, existing->account);
    }

    if (static_cast<int>(mappings.size()) >= options.max_sessions) {
      return "ERR limit the configured maximum number of session accounts is in use";
    }

    // The first free slot, so a purged account's name is reused rather than
    // the numbering drifting upward forever.
    int slot = 1;
    for (; slot <= options.max_sessions; ++slot) {
      const auto candidate = account_for_slot(slot);
      const bool mapped = std::ranges::any_of(mappings, [&](const auto &mapping) {
        return mapping.account == candidate;
      });
      // An account that exists without a mapping is left alone: it may be
      // debris from an interrupted purge, and reusing it would hand one
      // client another's home directory.
      if (!mapped && ::getpwnam(candidate.c_str()) == nullptr) {
        break;
      }
    }
    if (slot > options.max_sessions) {
      return "ERR limit no free session account slot is available";
    }

    const auto name = account_for_slot(slot);
    if (const auto failure = provision(options, name); failure) {
      std::fprintf(stderr, "could not provision %s: %s\n", name.c_str(), failure->c_str());
      return "ERR provision " + *failure;
    }

    mappings.push_back({client_id, name});
    if (!write_mappings(options, mappings)) {
      // The account exists but is unrecorded. Say so rather than report
      // success: a caller that retries would otherwise get a second account
      // for the same client.
      return "ERR state the account was created but the mapping could not be written";
    }
    return reply_for(options, name);
  }

  std::string handle_purge(const options_t &options, const std::string &client_id) {
    auto mappings = read_mappings(options);
    const auto existing = std::ranges::find_if(mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings.end()) {
      return "ERR notfound no account is mapped to that client";
    }

    const auto account = existing->account;
    const auto userdel = locate({"/usr/sbin/userdel", "/usr/bin/userdel"});
    if (userdel.empty()) {
      return "ERR provision userdel was not found";
    }

    // The mapping goes first. If the removal then fails halfway, the client
    // gets a fresh account on its next ENSURE instead of being handed a
    // half-deleted one.
    mappings.erase(existing);
    if (!write_mappings(options, mappings)) {
      return "ERR state the mapping could not be updated";
    }
    if (!run({userdel, "--remove", account})) {
      return "ERR provision userdel failed; the mapping was removed";
    }
    return "OK";
  }

  std::string handle_request(const options_t &options, const std::string &request) {
    const auto split = request.find(' ');
    const auto verb = trim(split == std::string::npos ? request : request.substr(0, split));
    const auto argument = split == std::string::npos ? std::string {} : trim(request.substr(split + 1));

    if (verb == "LIST") {
      std::string reply {"OK"};
      for (const auto &mapping : read_mappings(options)) {
        reply += ' ' + mapping.client_id + '=' + mapping.account;
      }
      return reply;
    }

    if (verb != "ENSURE" && verb != "LOOKUP" && verb != "PURGE") {
      return "ERR request unknown verb";
    }
    if (!valid_client_id(argument)) {
      return "ERR request the client identifier is missing or malformed";
    }

    if (verb == "ENSURE") {
      return handle_ensure(options, argument);
    }
    if (verb == "PURGE") {
      return handle_purge(options, argument);
    }

    const auto mappings = read_mappings(options);
    const auto existing = std::ranges::find_if(mappings, [&](const auto &mapping) {
      return mapping.client_id == argument;
    });
    if (existing == mappings.end()) {
      return "ERR notfound no account is mapped to that client";
    }
    return reply_for(options, existing->account);
  }
}  // namespace

namespace {
  std::optional<uid_t> peer_uid(int fd) {
    ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
      return std::nullopt;
    }
    return credentials.uid;
  }

  /**
   * @brief Whether `uid` may manage session accounts, per the root-owned allow file.
   *
   * One name or numeric uid per line, `#` starting a comment. Read per request
   * so granting or withdrawing access takes effect without a restart. The
   * credentials come from the kernel, not from the request, so a caller cannot
   * claim to be an allowed uid.
   */
  bool allowed(const options_t &options, uid_t uid) {
    std::ifstream stream {options.allow_file};
    if (!stream) {
      return false;
    }

    std::string line;
    while (std::getline(stream, line)) {
      const auto comment = line.find('#');
      const auto entry = trim(comment == std::string::npos ? line : line.substr(0, comment));
      if (entry.empty()) {
        continue;
      }
      if (entry == std::to_string(uid)) {
        return true;
      }
      if (const auto *account = ::getpwnam(entry.c_str()); account && account->pw_uid == uid) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::string> read_request(int fd) {
    std::string request;
    std::array<char, 512> buffer {};
    while (request.size() < 4096) {
      const auto count = ::read(fd, buffer.data(), buffer.size());
      if (count <= 0) {
        break;
      }
      request.append(buffer.data(), static_cast<size_t>(count));
      if (const auto newline = request.find('\n'); newline != std::string::npos) {
        return request.substr(0, newline);
      }
    }
    return request.empty() ? std::nullopt : std::optional {request};
  }

  void write_reply(int fd, const std::string &reply) {
    const auto line = reply + '\n';
    size_t offset = 0;
    while (offset < line.size()) {
      const auto count = ::write(fd, line.data() + offset, line.size() - offset);
      if (count <= 0) {
        return;
      }
      offset += static_cast<size_t>(count);
    }
  }

  void serve(const options_t &options, int connection) {
    const auto uid = peer_uid(connection);
    if (!uid) {
      write_reply(connection, "ERR denied the peer's credentials are unreadable");
      return;
    }
    if (!allowed(options, *uid)) {
      std::fprintf(stderr, "refused a request from uid %u; add it to %s to allow it\n",
                   static_cast<unsigned int>(*uid), options.allow_file.c_str());
      write_reply(connection, "ERR denied this user may not manage session accounts");
      return;
    }

    const auto request = read_request(connection);
    if (!request) {
      write_reply(connection, "ERR request no request was received");
      return;
    }

    // Slot allocation reads what exists and then creates, so two clients
    // arriving together could otherwise be given the same account.
    const int lock_fd = ::open(options.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0) {
      ::flock(lock_fd, LOCK_EX);
    }
    const auto reply = handle_request(options, *request);
    if (lock_fd >= 0) {
      ::close(lock_fd);  // Releases the lock.
    }

    write_reply(connection, reply);
  }

  int inherited_listen_fd() {
    const char *listen_pid = ::getenv("LISTEN_PID");
    const char *listen_fds = ::getenv("LISTEN_FDS");
    if (listen_pid && listen_fds && std::strtol(listen_pid, nullptr, 10) == ::getpid() &&
        std::strtol(listen_fds, nullptr, 10) >= 1) {
      return SD_LISTEN_FDS_START;
    }
    return -1;
  }
}  // namespace

int main(int argc, char *argv[]) {
  options_t options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view flag {argv[index]};
    const auto value = [&]() -> std::string {
      return index + 1 < argc ? argv[++index] : std::string {};
    };
    if (flag == "--allow-file") {
      options.allow_file = value();
    } else if (flag == "--state-file") {
      options.state_file = value();
    } else if (flag == "--home-root") {
      options.home_root = value();
    } else if (flag == "--lock-file") {
      options.lock_file = value();
    } else if (flag == "--max-sessions") {
      const auto requested = std::atoi(value().c_str());
      if (requested < 1 || requested > session_ceiling) {
        std::fprintf(stderr, "--max-sessions must be between 1 and %d\n", session_ceiling);
        return 2;
      }
      options.max_sessions = requested;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--allow-file PATH] [--state-file PATH] [--home-root PATH]\n"
                   "          [--lock-file PATH] [--max-sessions N]\n",
                   argv[0]);
      return 2;
    }
  }

  // A closed peer must not take the broker down with it.
  ::signal(SIGPIPE, SIG_IGN);

  const int listener = inherited_listen_fd();
  if (listener < 0) {
    std::fprintf(stderr, "this service expects a socket-activated listener from systemd\n");
    return 1;
  }

  for (;;) {
    const int connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "accept failed: %s\n", std::strerror(errno));
      return 1;
    }
    serve(options, connection);
    ::close(connection);
  }
}
