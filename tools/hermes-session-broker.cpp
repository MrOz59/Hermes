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
 * socket, speaking a line protocol:
 *
 *     ENSURE <client-uuid>  -> OK <user> <uid> <home>
 *     LOOKUP <client-uuid>  -> OK <user> <uid> <home>
 *     LIST                  -> OK <client-uuid>=<user>...
 *     PURGE <client-uuid>   -> OK
 *     START <client-uuid>   -> OK <unit>
 *       ARG <word>...         one line each, taken literally
 *       ENV <KEY>=<VALUE>...  one line each, taken literally
 *       SCOPE <suffix>        optional; a second unit beside the session's
 *       END
 *     STOP  <client-uuid>   -> OK
 *     STATUS <client-uuid>  -> OK <systemd ActiveState> <unit>
 *     SOCKET <client-uuid>  -> OK <wayland socket name>
 *
 * START is the only request that carries more than a line, because a session's
 * command and environment do not fit on one and splitting them on whitespace
 * would be a quoting bug waiting to happen. Every ARG and ENV line is used
 * whole, with one exception: an ENV whose name systemd cannot carry - a bash
 * exported function being the everyday case - is dropped with a warning to the
 * journal, because refusing the whole session over one shell artefact would
 * leave that machine unable to launch isolated sessions at all.
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
 * START does not run the session itself. It asks systemd for a transient unit
 * with `User=` set to the session account, which is the difference between a
 * session that merely runs as somebody else and one that is actually contained:
 * a unit started by PID 1 gets its own cgroup, its own sandboxing and its own
 * lifetime, where a process this broker forked would instead inherit this
 * unit's - a network namespace with no network, a `MemoryDenyWriteExecute` that
 * stops Wine from ever JITting, a hidden /home. The session outliving this
 * broker is a feature too: systemd reaps it, so a broker restart is not a
 * dropped desktop.
 *
 * `systemd-run` is asked rather than the bus method called directly, for the
 * same reason useradd is asked rather than /etc/passwd written: it already
 * owns the correct call, and reimplementing it is a class of bug worth not
 * owning. The session needs `/run/user/<uid>` and a user manager to have a
 * session bus at all, which is what lingering gets it; it does not need a
 * logind session, because its compositor takes the seat through seatd.
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
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

  // How long one connection may take over its socket. The broker accepts
  // serially, so a client that connects and says nothing stalls every client
  // behind it; this deadline is what turns that stall from forever into a
  // bounded wait. It covers reads and writes alike, and a full request has to
  // arrive within it - the ceiling below bounds size, this bounds time.
  constexpr auto socket_timeout = std::chrono::seconds(10);

  // STATUS asks systemd for a unit's state, which is a fork and exec of
  // `systemctl show`. Callers poll it - the Game Mode console every couple of
  // seconds, the streaming host while a session is alive - so the answer is
  // cached briefly. A state is stale after this long at worst, which no caller
  // can distinguish from a poll that has not happened yet.
  constexpr auto status_cache_ttl = std::chrono::seconds(2);

  struct status_cache_t {
    struct entry_t {
      std::chrono::steady_clock::time_point expires_at;
      std::string state;
    };

    // The broker serves one connection at a time, so no lock is needed.
    std::unordered_map<std::string, entry_t> entries;

    std::optional<std::string> find(const std::string &unit) {
      const auto it = entries.find(unit);
      if (it == entries.end()) {
        return std::nullopt;
      }
      if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        entries.erase(it);
        return std::nullopt;
      }
      return it->second.state;
    }

    void store(const std::string &unit, std::string state) {
      entries[unit] = {std::chrono::steady_clock::now() + status_cache_ttl, std::move(state)};
    }
  };

  status_cache_t status_cache;

  /**
   * @brief Wait until `fd` is ready for `events` or the deadline passes.
   *
   * True when the descriptor is ready (or hung up, which the read or write
   * that follows reports precisely); false when the deadline passed first.
   * This is the whole timeout mechanism: every read and write on a connection
   * goes through this gate, so no peer can hold the broker by simply never
   * sending or never reading.
   */
  bool wait_ready(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return false;
      }
      pollfd descriptor {fd, events, 0};
      const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
      if (ready > 0) {
        return (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
      }
      if (ready == 0) {
        return false;
      }
      if (errno != EINTR) {
        return false;
      }
    }
  }

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

  /**
   * @brief Whether an ENV line is an assignment systemd will accept.
   *
   * The caller is the trusted Hermes user, so this is not a privilege boundary;
   * it is a boundary against a malformed environment reaching `systemd-run` as
   * something that parses as an option instead, and against a name systemd
   * will refuse outright: `--setenv` accepts only the classic alphanumeric
   * plus-underscore names, so an exported bash function (a `BASH_FUNC_foo%%=`
   * entry) cannot be carried at all, by anyone. A line that fails this check
   * is dropped with a warning rather than failing the START - a session must
   * not be refused because the host's shell exports a function.
   */
  bool valid_env_assignment(std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0 || line.size() > 4096) {
      return false;
    }
    const auto name = line.substr(0, equals);
    if (std::isdigit(static_cast<unsigned char>(name.front()))) {
      return false;
    }
    return std::ranges::all_of(name, [](unsigned char ch) {
      return std::isalnum(ch) || ch == '_';
    });
  }

  /**
   * @brief The unit name a session account's service takes.
   *
   * Derived from the account, which is itself derived from a slot, so a unit
   * name is a fixed shape no caller ever chooses.
   */
  std::string unit_for_account(const std::string &account) {
    return "hermes-session-" + account.substr(8) + ".service";
  }

  /**
   * @brief A second unit beside the session's, for what runs inside it.
   *
   * A desktop application is started after the compositor and has to be a unit
   * of its own, because the session's name is taken. It is bound to the
   * session's with PartOf, so stopping the session stops it too - which is what
   * the process group used to do back when everything was a child of Hermes.
   */
  bool valid_scope(std::string_view scope) {
    if (scope.empty() || scope.size() > 16) {
      return false;
    }
    return std::ranges::all_of(scope, [](unsigned char ch) {
      return std::islower(ch) || std::isdigit(ch);
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
   *
   * A line that does not parse fails the whole read. Dropping it instead is
   * silent data loss with somebody's save games as the blast radius: the next
   * write persists the file without that line, so the client it belonged to
   * comes back, matches nothing, and is handed a fresh account while its home
   * sits unreferenced on disk. The file is root's, so a line that does not
   * parse is damage, and damage is worth refusing to act on.
   */
  std::optional<std::vector<mapping_t>> read_mappings(const options_t &options) {
    std::vector<mapping_t> mappings;
    std::ifstream stream {options.state_file};
    if (!stream) {
      return mappings;  // No file yet is an empty map, not damage.
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
        return std::nullopt;
      }
      mapping_t mapping {trim(entry.substr(0, split)), trim(entry.substr(split + 1))};
      if (!valid_client_id(mapping.client_id) || !valid_account_name(mapping.account)) {
        return std::nullopt;
      }
      mappings.emplace_back(std::move(mapping));
    }
    return mappings;
  }

  bool write_mappings(const options_t &options, const std::vector<mapping_t> &mappings) {
    std::error_code ec;
    fs::create_directories(fs::path {options.state_file}.parent_path(), ec);

    // Written through a temporary and renamed, so a crash cannot leave the
    // broker with a half-written idea of who owns which account. The rename is
    // only half of that guarantee: without the fsync below, the rename can
    // reach the disk before the bytes do, and the crash that follows leaves a
    // map that is present, empty and authoritative.
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
    if (const int fd = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
      const bool durable = ::fsync(fd) == 0;
      ::close(fd);
      if (!durable) {
        ::unlink(temporary.c_str());
        return false;
      }
    }
    fs::rename(temporary, options.state_file, ec);
    if (ec) {
      ::unlink(temporary.c_str());
      return false;
    }
    // And the directory entry itself, so the rename survives the same crash.
    const auto directory = fs::path {options.state_file}.parent_path();
    if (const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
      ::fsync(fd);
      ::close(fd);
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
   * @brief Run a command and collect its standard output.
   *
   * Only `systemctl show` needs this, and it needs it because the question it
   * answers - what state is this unit in - has more than two answers. Asking
   * `is-active` instead would collapse them into a yes/no and report a unit
   * that is still starting as one that is not running at all, which for a
   * compositor is the difference between "wait" and "it died".
   */
  bool run_capture(const std::vector<std::string> &argv, std::string &output) {
    int pipe_fds[2] {};
    if (::pipe(pipe_fds) != 0) {
      return false;
    }

    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &argument : argv) {
      raw.push_back(const_cast<char *>(argument.c_str()));
    }
    raw.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return false;
    }
    if (child == 0) {
      ::close(pipe_fds[0]);
      ::dup2(pipe_fds[1], STDOUT_FILENO);
      ::close(pipe_fds[1]);
      ::execv(raw[0], raw.data());
      ::_exit(127);
    }

    ::close(pipe_fds[1]);
    std::array<char, 512> buffer {};
    for (;;) {
      const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
      if (count <= 0) {
        break;
      }
      output.append(buffer.data(), static_cast<size_t>(count));
    }
    ::close(pipe_fds[0]);

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
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing != mappings->end()) {
      // A mapping whose account has gone - a purge that got as far as userdel,
      // an administrator tidying passwd by hand - would otherwise strand this
      // client for good: it matches a name that cannot be resolved, so it never
      // reaches the provisioning below and every reply is an error. Recreate it
      // under the name it already holds, which is also the slot it already
      // occupies, rather than leaking a second one.
      if (::getpwnam(existing->account.c_str()) == nullptr) {
        std::fprintf(stderr, "recreating %s, which the map claims but passwd does not\n",
                     existing->account.c_str());
        if (const auto failure = provision(options, existing->account); failure) {
          return "ERR provision " + *failure;
        }
      }
      return reply_for(options, existing->account);
    }

    if (static_cast<int>(mappings->size()) >= options.max_sessions) {
      return "ERR limit the configured maximum number of session accounts is in use";
    }

    // The first free slot, so a purged account's name is reused rather than
    // the numbering drifting upward forever.
    int slot = 1;
    for (; slot <= options.max_sessions; ++slot) {
      const auto candidate = account_for_slot(slot);
      const bool mapped = std::ranges::any_of(*mappings, [&](const auto &mapping) {
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

    mappings->push_back({client_id, name});
    if (!write_mappings(options, *mappings)) {
      // The account exists but is unrecorded. Say so rather than report
      // success: a caller that retries would otherwise get a second account
      // for the same client.
      return "ERR state the account was created but the mapping could not be written";
    }
    return reply_for(options, name);
  }

  // Defined below, beside handle_stop; PURGE needs it first to stop the
  // session ahead of userdel.
  std::optional<std::string> stop_session(const options_t &options, const std::string &account);

  std::string handle_purge(const options_t &options, const std::string &client_id) {
    auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings->end()) {
      return "ERR notfound no account is mapped to that client";
    }

    const auto account = existing->account;
    const auto userdel = locate({"/usr/sbin/userdel", "/usr/bin/userdel"});
    if (userdel.empty()) {
      return "ERR provision userdel was not found";
    }

    // The session is stopped before the account is removed: userdel refuses
    // an account with live processes, and a home deleted out from under a
    // running compositor corrupts the files it is writing. A failure to stop
    // is not the end - userdel then reports the truth of what is left.
    if (const auto failure = stop_session(options, existing->account); failure) {
      std::fprintf(stderr, "could not stop %s before purging it: %s\n",
                   existing->account.c_str(), failure->c_str());
    }

    // The account goes first and the mapping second. The other order loses a
    // slot for good on a failed userdel: the account survives with nothing
    // pointing at it, and the slot search below skips every name that already
    // exists in passwd, so nobody is ever given that one again. This way a
    // failed removal changes nothing, and a failed map write leaves a mapping
    // that ENSURE knows how to repair. The lookup makes a repeat purge a
    // no-op rather than an error out of userdel.
    if (::getpwnam(account.c_str()) != nullptr && !run({userdel, "--remove", account})) {
      return "ERR provision userdel failed; nothing was changed";
    }
    mappings->erase(existing);
    if (!write_mappings(options, *mappings)) {
      return "ERR state the account was removed but the mapping could not be updated";
    }
    return "OK";
  }

  /**
   * @brief Ask systemd for a transient unit running `argv` as the client's account.
   *
   * The unit, not this process, is what contains the session: PID 1 starts it,
   * so it gets a cgroup and a sandbox of its own rather than inheriting the
   * confinement this broker is under - which forbids a network, forbids
   * writable-executable memory, and hides /home, none of which a desktop
   * survives.
   *
   * Lingering comes first because a session with no `/run/user/<uid>` has
   * nowhere to put a session bus, a Wayland socket or a PipeWire socket, and
   * systemd only creates it for a user with a manager running. The compositor's
   * seat does not come from here - it takes that through seatd - so no logind
   * session and no PAM stack is involved.
   */
  std::string handle_start(
    const options_t &options,
    const std::string &client_id,
    const std::vector<std::string> &arguments,
    const std::vector<std::string> &environment,
    const std::string &scope
  ) {
    if (arguments.empty()) {
      return "ERR request START needs at least one ARG";
    }
    if (!arguments.front().starts_with('/')) {
      return "ERR request the first ARG must be an absolute path";
    }

    const auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings->end()) {
      return "ERR notfound no account is mapped to that client; ENSURE it first";
    }
    const auto *account = ::getpwnam(existing->account.c_str());
    if (account == nullptr) {
      return "ERR state the mapped account no longer exists";
    }

    const auto systemd_run = locate({"/usr/bin/systemd-run", "/bin/systemd-run"});
    if (systemd_run.empty()) {
      return "ERR provision systemd-run was not found";
    }
    const auto loginctl = locate({"/usr/bin/loginctl", "/bin/loginctl"});
    if (loginctl.empty()) {
      return "ERR provision loginctl was not found";
    }
    if (!run({loginctl, "enable-linger", existing->account})) {
      return "ERR provision lingering could not be enabled for the session account";
    }

    const auto uid = std::to_string(static_cast<unsigned>(account->pw_uid));
    const auto session_unit = unit_for_account(existing->account);
    const auto unit = scope.empty() ?
                        session_unit :
                        session_unit.substr(0, session_unit.size() - 8) + '-' + scope + ".service";
    std::vector<std::string> argv {
      systemd_run,
      "--quiet",
      // Without --collect a session that exits non-zero stays behind in the
      // failed state, and the next START of the same slot collides with it.
      "--collect",
      "--unit=" + unit,
      "--uid=" + existing->account,
      "--description=Hermes isolated session for " + existing->account,
      "--property=WorkingDirectory=" + std::string {account->pw_dir},
    };
    if (!scope.empty()) {
      // Bound to the session rather than merely started after it: stopping the
      // session has to take this with it, the way terminating the process group
      // used to when everything was a child of Hermes.
      argv.push_back("--property=PartOf=" + session_unit);
      argv.push_back("--property=After=" + session_unit);
    }
    // The caller's environment goes on first so the three values below win. A
    // session whose XDG_RUNTIME_DIR did not match its uid would find the
    // directory unwritable and fail in a way that reads like a compositor bug.
    for (const auto &assignment : environment) {
      argv.push_back("--setenv=" + assignment);
    }
    argv.push_back("--setenv=XDG_RUNTIME_DIR=/run/user/" + uid);
    // HOME, USER, LOGNAME and SHELL come from the passwd entry, because systemd
    // sets those itself for a unit with User=.
    argv.emplace_back("--");
    argv.insert(argv.end(), arguments.begin(), arguments.end());

    if (!run(argv)) {
      std::fprintf(stderr, "systemd-run refused to start %s\n", unit.c_str());
      return "ERR provision systemd-run refused to start the session";
    }
    // The unit is coming up; a STATUS right behind this START must not be
    // answered from a cache entry that still remembers it as gone.
    status_cache.store(session_unit, "activating");
    return "OK " + unit;
  }

  /**
   * @brief Whether the client's session unit is running.
   *
   * Hermes used to answer this by asking the child process it had forked. A
   * unit has no such handle on this side, and systemd is the only thing that
   * knows, so the question comes here rather than being guessed at from a pid
   * file or a socket that may outlive its owner.
   */
  std::string handle_status(const options_t &options, const std::string &client_id) {
    const auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings->end()) {
      return "ERR notfound no account is mapped to that client";
    }

    const auto unit = unit_for_account(existing->account);

    // STATUS is polled - the Game Mode console every couple of seconds, the
    // streaming host while a session is alive - and each answer otherwise
    // costs a fork and exec of `systemctl show`. The state is cached briefly:
    // nothing systemd knows changes between two polls that close together.
    std::string state;
    if (const auto cached = status_cache.find(unit); cached) {
      state = *cached;
    } else {
      const auto systemctl = locate({"/usr/bin/systemctl", "/bin/systemctl"});
      if (systemctl.empty()) {
        return "ERR provision systemctl was not found";
      }
      if (!run_capture({systemctl, "show", "--property=ActiveState", "--value", unit}, state)) {
        return "ERR provision the session unit's state could not be read";
      }
      state = trim(state);
      status_cache.store(unit, state);
    }
    // systemd answers `inactive` for a unit it has never heard of, which is
    // the right answer to this question: nothing of that name is running.
    return "OK " + (state.empty() ? std::string {"inactive"} : state) + ' ' + unit;
  }

  /**
   * @brief The Wayland socket a session's compositor created, if it has yet.
   *
   * Hermes used to find this by listing the session's runtime directory. Once
   * the session belongs to another user that directory is 0700 and somebody
   * else's, so the listing has to happen here - the one place that can read it -
   * and only the name comes back. A name is all Hermes needs: what connects to
   * the socket is launched into the session, not run by Hermes.
   */
  std::string handle_socket(const options_t &options, const std::string &client_id) {
    const auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings->end()) {
      return "ERR notfound no account is mapped to that client";
    }
    const auto *account = ::getpwnam(existing->account.c_str());
    if (account == nullptr) {
      return "ERR state the mapped account no longer exists";
    }

    const auto runtime_dir = "/run/user/" + std::to_string(static_cast<unsigned>(account->pw_uid));
    std::vector<std::string> candidates;
    std::error_code iteration_error;
    for (fs::directory_iterator it {runtime_dir, iteration_error}, end;
         !iteration_error && it != end; it.increment(iteration_error)) {
      const auto name = it->path().filename().string();
      // A status error is this entry's alone and must not end the scan: the
      // code is local to the iteration, where sharing one with the iterator
      // would abort the loop on the first unreadable entry and hide every
      // socket that came after it.
      std::error_code status_error;
      if ((name.starts_with("gamescope-") || name.starts_with("wayland-")) &&
          fs::is_socket(it->symlink_status(status_error))) {
        candidates.emplace_back(name);
      }
    }
    if (candidates.empty()) {
      return "ERR notfound the session has not created a Wayland socket";
    }
    std::ranges::sort(candidates);
    return "OK " + candidates.front();
  }

  /**
   * @brief Stop the account's session unit and turn lingering off.
   *
   * Shared by STOP and by PURGE ahead of userdel: an account whose session is
   * still running cannot be removed at all - userdel refuses while its
   * processes exist - and deleting the home out from under a running
   * compositor would corrupt whatever it is writing.
   */
  std::optional<std::string> stop_session(const options_t &options, const std::string &account) {
    const auto systemctl = locate({"/usr/bin/systemctl", "/bin/systemctl"});
    if (systemctl.empty()) {
      return "systemctl was not found";
    }
    // Asked for first, because `systemctl stop` fails on a unit it cannot find
    // and a stopped session's unit is gone - `--collect` removes it. A second
    // STOP, or one for a session that ended on its own, is a request for a
    // state that already holds, so it skips the stop instead of reporting a
    // failure to reach it.
    const auto unit = unit_for_account(account);
    std::string state;
    run_capture({systemctl, "show", "--property=ActiveState", "--value", unit}, state);
    state = trim(state);
    if (!state.empty() && state != "inactive" && state != "failed") {
      if (!run({systemctl, "stop", unit})) {
        return "the session unit could not be stopped";
      }
      state = "inactive";
    }
    // The state the unit is now in is the state a STATUS behind this STOP
    // must report, not whatever a cache entry from before the stop remembers.
    status_cache.store(unit, state.empty() ? "inactive" : state);

    // Lingering is what START turned on to get the account a runtime directory
    // and a user manager, and it survives reboots. Left on, every client that
    // ever streamed would keep a manager and a session bus running on the host
    // forever. The account and its home are untouched - only the machinery of a
    // session that is over goes - so the next START brings it all back.
    if (const auto loginctl = locate({"/usr/bin/loginctl", "/bin/loginctl"}); !loginctl.empty()) {
      run({loginctl, "disable-linger", account});
    }
    return std::nullopt;
  }

  std::string handle_stop(const options_t &options, const std::string &client_id) {
    const auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == client_id;
    });
    if (existing == mappings->end()) {
      return "ERR notfound no account is mapped to that client";
    }

    if (const auto failure = stop_session(options, existing->account); failure) {
      return "ERR provision " + *failure;
    }
    return "OK";
  }

  std::string handle_request(const options_t &options, const std::vector<std::string> &lines) {
    const auto &request = lines.front();
    const auto split = request.find(' ');
    const auto verb = trim(split == std::string::npos ? request : request.substr(0, split));
    const auto argument = split == std::string::npos ? std::string {} : trim(request.substr(split + 1));

    if (verb == "LIST") {
      const auto mappings = read_mappings(options);
      if (!mappings) {
        return "ERR state the account map is damaged; refusing to act on it";
      }
      std::string reply {"OK"};
      for (const auto &mapping : *mappings) {
        reply += ' ' + mapping.client_id + '=' + mapping.account;
      }
      return reply;
    }

    if (verb != "ENSURE" && verb != "LOOKUP" && verb != "PURGE" && verb != "START" &&
        verb != "STOP" && verb != "STATUS" && verb != "SOCKET") {
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
    if (verb == "STOP") {
      return handle_stop(options, argument);
    }
    if (verb == "STATUS") {
      return handle_status(options, argument);
    }
    if (verb == "SOCKET") {
      return handle_socket(options, argument);
    }
    if (verb == "START") {
      std::vector<std::string> arguments;
      std::vector<std::string> environment;
      std::string scope;
      for (size_t index = 1; index < lines.size(); ++index) {
        const auto &line = lines[index];
        if (line == "END") {
          break;
        }
        if (line.starts_with("SCOPE ")) {
          scope = line.substr(6);
          if (!valid_scope(scope)) {
            return "ERR request the SCOPE is not a short lowercase word";
          }
          continue;
        }
        if (line.starts_with("ARG ")) {
          if (arguments.size() >= 256) {
            return "ERR request too many ARG lines";
          }
          arguments.emplace_back(line.substr(4));
        } else if (line.starts_with("ENV ")) {
          if (environment.size() >= 512) {
            return "ERR request too many ENV lines";
          }
          const auto assignment = line.substr(4);
          if (!valid_env_assignment(assignment)) {
            // One bad line must not take the whole launch down, and the warning
            // must say which line it was. Only the name is logged - the value
            // is the caller's data, and a function body is not worth echoing.
            const auto name = assignment.substr(0, assignment.find('='));
            std::fprintf(stderr, "dropping ENV line: %.*s is not a name systemd accepts\n",
                         static_cast<int>(name.size()), name.data());
            continue;
          }
          environment.emplace_back(assignment);
        } else {
          return "ERR request a START block takes only ARG, ENV, SCOPE and END lines";
        }
      }
      return handle_start(options, argument, arguments, environment, scope);
    }

    const auto mappings = read_mappings(options);
    if (!mappings) {
      return "ERR state the account map is damaged; refusing to act on it";
    }
    const auto existing = std::ranges::find_if(*mappings, [&](const auto &mapping) {
      return mapping.client_id == argument;
    });
    if (existing == mappings->end()) {
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

  /**
   * @brief Whether everything the caller means to send has arrived.
   *
   * Every request but START is one line. START carries a command and an
   * environment, so it is a block terminated by a line that is exactly `END` -
   * which is also what stops a caller that opens a connection and says nothing
   * from being read forever.
   */
  bool request_is_complete(const std::string &buffer) {
    const auto first = buffer.find('\n');
    if (first == std::string::npos) {
      return false;
    }
    if (!trim(buffer.substr(0, first)).starts_with("START")) {
      return true;
    }
    for (size_t start = 0; start < buffer.size();) {
      const auto end = buffer.find('\n', start);
      if (end == std::string::npos) {
        return false;
      }
      if (trim(buffer.substr(start, end - start)) == "END") {
        return true;
      }
      start = end + 1;
    }
    return false;
  }

  std::optional<std::vector<std::string>> read_request(int fd) {
    // Large enough for a desktop's environment, small enough that a caller
    // cannot make the broker hold a request it will never finish sending.
    // The ceiling bounds size; socket_timeout, the deadline every read below
    // is gated by, bounds time - the two together are what a request must
    // fit inside.
    constexpr size_t request_ceiling = 64 * 1024;

    const auto deadline = std::chrono::steady_clock::now() + socket_timeout;
    std::string buffer;
    std::array<char, 4096> chunk {};
    while (buffer.size() < request_ceiling && !request_is_complete(buffer)) {
      if (!wait_ready(fd, POLLIN, deadline)) {
        std::fprintf(stderr, "a connection sent no complete request within %lld seconds; dropping it\n",
                     static_cast<long long>(socket_timeout.count()));
        return std::nullopt;
      }
      const auto count = ::read(fd, chunk.data(), chunk.size());
      if (count <= 0) {
        break;
      }
      buffer.append(chunk.data(), static_cast<size_t>(count));
    }
    if (!request_is_complete(buffer)) {
      return std::nullopt;
    }

    std::vector<std::string> lines;
    for (size_t start = 0; start < buffer.size();) {
      const auto end = buffer.find('\n', start);
      if (end == std::string::npos) {
        break;
      }
      // The verb line is trimmed, the same way the completeness check above
      // reads it. Every line after it loses only a trailing carriage return, so
      // an ARG or ENV value arrives exactly as it was sent - leading spaces in
      // a path or a value included.
      auto line = buffer.substr(start, end - start);
      if (lines.empty()) {
        line = trim(line);
      } else if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(std::move(line));
      if (lines.back() == "END" || !lines.front().starts_with("START")) {
        break;
      }
      start = end + 1;
    }
    return lines.empty() ? std::nullopt : std::optional {lines};
  }

  void write_reply(int fd, const std::string &reply) {
    const auto line = reply + '\n';
    const auto deadline = std::chrono::steady_clock::now() + socket_timeout;
    size_t offset = 0;
    while (offset < line.size()) {
      // A peer that stops reading must not hold the broker open either; the
      // same deadline guards this half of the conversation.
      if (!wait_ready(fd, POLLOUT, deadline)) {
        return;
      }
      const auto count = ::write(fd, line.data() + offset, line.size() - offset);
      if (count <= 0) {
        return;
      }
      offset += static_cast<size_t>(count);
    }
  }

  /**
   * @brief Whether the request allocates or frees a session account slot.
   *
   * These are the requests the lock file exists for, and the only ones that
   * serialize on it. Everything else - STATUS in particular, which callers
   * poll every couple of seconds - is served without it, so a poll never has
   * to wait behind another client's useradd, and a launch never waits behind
   * a poll.
   */
  bool request_mutates(const std::vector<std::string> &request) {
    const auto &line = request.front();
    const auto split = line.find(' ');
    const auto verb = trim(split == std::string::npos ? line : line.substr(0, split));
    return verb == "ENSURE" || verb == "PURGE";
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
      write_reply(connection, "ERR request no complete request was received");
      return;
    }

    // Slot allocation reads what exists and then creates, so two clients
    // arriving together could otherwise be given the same account. The lock
    // is cross-process (a socket-activated second instance is its own
    // process), which is why it is a file rather than a local variable.
    int lock_fd = -1;
    if (request_mutates(*request)) {
      lock_fd = ::open(options.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (lock_fd >= 0) {
        ::flock(lock_fd, LOCK_EX);
      }
    }
    const auto reply = handle_request(options, *request);
    if (lock_fd >= 0) {
      ::close(lock_fd);  // Releases the lock.
    }

    write_reply(connection, reply);
  }

  /**
   * @brief Keep a descriptor out of the tools this broker execs.
   *
   * useradd and userdel inherit whatever is open at fork. systemd hands its
   * listener over without FD_CLOEXEC by design - the service has to survive its
   * own exec - and accept() copies that omission onto every connection. Neither
   * has any business in a child: a useradd that hung would hold the listening
   * socket open past the broker's own life, and hold a caller's connection open
   * with it.
   */
  void keep_from_children(int fd) {
    if (const int flags = ::fcntl(fd, F_GETFD); flags >= 0) {
      ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
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
  keep_from_children(listener);

  for (;;) {
    const int connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "accept failed: %s\n", std::strerror(errno));
      return 1;
    }
    keep_from_children(connection);
    serve(options, connection);
    ::close(connection);
  }
}
