// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file tools/hermes-kms-card-broker.cpp
 * @brief Create and remove Hermes-KMS cards for an unprivileged Hermes.
 *
 * Hermes-KMS gained runtime card creation through configfs, which lives under
 * /sys/kernel/config and is therefore root's. Hermes runs as the user whose
 * desktop it streams, so it cannot write there and must ask something that
 * can. This is that something: a small root service reachable over one unix
 * socket, speaking a line protocol with four requests and no state of its own.
 *
 *     CREATE                -> OK <name> <card> <render_node>
 *     CREATE <uid>          -> card owned by that Hermes session account
 *     REMOVE <name>         -> OK
 *     LIST                  -> OK <name>...
 *     SWEEP                 -> OK <removed-count>
 *
 * Authorization is the peer's uid, taken from SO_PEERCRED (which the kernel
 * fills in, so it cannot be spoofed) and checked against an allow file that
 * only root can write. A card is named hermes-u<peer-uid>-<index> - so a
 * request can only ever remove a card the same uid created, and never a
 * statically configured one - while its access_uid is the uid the card
 * actually belongs to: the caller's own for a bare CREATE, or, for
 * `CREATE <uid>`, a Hermes session account's. That form is what an isolated
 * session needs: the session's compositor runs as hermes-sNN, and the
 * driver's udev rules hand the card's device nodes to its access_uid, so a
 * card created with the host user's uid is one the session can never open.
 * The gate is the hermes-session group: an account outside it cannot be
 * named, which keeps a caller in the allow file from minting cards for
 * arbitrary users.
 *
 * The session index is allocated here rather than requested, because it is not
 * a private number. The driver's udev rules turn it into the private DRM seat
 * `hermes-kms-<index>` and the seatd broker instance that owns it, so two
 * cards sharing an index would share a seat -- exactly the isolation the pool
 * exists to provide. Allocation reads the indices in use from sysfs, covering
 * statically configured cards as well as created ones, and runs under a lock
 * so two clients cannot pick the same free slot.
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

  constexpr const char *default_socket_path = "/run/hermes/card-broker.sock";
  constexpr const char *default_allow_file = "/etc/hermes/card-broker.allow";
  constexpr const char *default_configfs_root = "/sys/kernel/config/hermes-kms";
  constexpr const char *default_platform_root = "/sys/devices/platform";
  constexpr const char *default_lock_file = "/run/hermes/card-broker.lock";

  // The group every isolated session account is created in, by the sysusers
  // file that creates the group. `CREATE <uid>` may only name an account in
  // it: any wider gate would let a caller in the allow file hand cards to
  // arbitrary users.
  constexpr const char *session_group = "hermes-session";

  // The driver maps session indices 1..8 onto private seats; there is no ninth
  // seat rule, so a ninth card would land on seat0 with the host compositor.
  constexpr unsigned int max_session_index = 8;
  // One user cannot occupy every seat by itself and lock everyone else out.
  constexpr unsigned int default_max_cards_per_uid = 4;
  // A request is a short command line; anything longer is not one.
  constexpr size_t max_request_bytes = 512;
  // Where systemd puts the first socket it passes down.
  constexpr int SD_LISTEN_FDS_START = 3;

  // How long one connection may take over its socket. The broker accepts
  // serially, so a client that connects and says nothing stalls every client
  // behind it; this deadline is what turns that stall from forever into a
  // bounded wait, on both the request and the reply half of the conversation.
  constexpr auto socket_timeout = std::chrono::seconds(10);

  volatile sig_atomic_t stopping = 0;

  void stop_handler(int) {
    stopping = 1;
  }

  /**
   * @brief Wait until `fd` is ready for `events` or the deadline passes.
   *
   * True when the descriptor is ready (or hung up, which the read or write
   * that follows reports precisely); false when the deadline passed first.
   * Every read and write on a connection goes through this gate, so no peer
   * can hold the broker by simply never sending or never reading.
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
    std::string socket_path {default_socket_path};
    std::string allow_file {default_allow_file};
    std::string configfs_root {default_configfs_root};
    std::string platform_root {default_platform_root};
    std::string lock_file {default_lock_file};
    unsigned int max_cards_per_uid {default_max_cards_per_uid};
  };

  std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
      return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string {text.substr(begin, end - begin + 1)};
  }

  std::optional<std::string> read_file(const fs::path &path) {
    std::ifstream stream {path};
    if (!stream) {
      return std::nullopt;
    }
    std::string value;
    std::getline(stream, value);
    return trim(value);
  }

  bool write_file(const fs::path &path, std::string_view value) {
    // configfs attributes are written whole, in one call; an ofstream would be
    // free to split the value across writes.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    const ssize_t written = ::write(fd, value.data(), value.size());
    const bool ok = written == static_cast<ssize_t>(value.size());
    ::close(fd);
    return ok;
  }

  /**
   * @brief Session indices already spoken for, from every Hermes-KMS card the
   *        kernel knows about, statically configured or created here.
   *
   * Nothing when sysfs could not be read at all. That is deliberately not an
   * empty result: a seat the broker failed to see is not a seat it may hand
   * out, because two cards sharing an index share a seat - handing one out is
   * exactly the collision the pool exists to prevent. Allocation refuses to
   * guess.
   */
  std::optional<std::vector<bool>> used_session_indices(const options_t &options) {
    std::vector<bool> used(max_session_index + 1, false);
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(options.platform_root, error)) {
      const auto name = entry.path().filename().string();
      if (name.rfind("hermes-kms", 0) != 0) {
        continue;
      }
      const auto index = read_file(entry.path() / "hermes_kms_session_index");
      if (!index) {
        continue;
      }
      const auto value = std::strtoul(index->c_str(), nullptr, 10);
      if (value >= 1 && value <= max_session_index) {
        used[value] = true;
      }
    }
    if (error) {
      return std::nullopt;
    }
    return used;
  }

  std::string card_name(uid_t uid, unsigned int session_index) {
    return "hermes-u" + std::to_string(uid) + "-" + std::to_string(session_index);
  }

  /** @brief Whether `name` is a card this broker created for `uid`. */
  bool owned_by(const std::string &name, uid_t uid) {
    const std::string prefix = "hermes-u" + std::to_string(uid) + "-";
    if (name.size() <= prefix.size() || name.rfind(prefix, 0) != 0) {
      return false;
    }
    const auto suffix = name.substr(prefix.size());
    return !suffix.empty() &&
           std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); });
  }

  /**
   * @brief Whether `uid` is a Hermes session account.
   *
   * The gate on `CREATE <uid>`: the account must exist and be in the
   * hermes-session group, either as its primary group or as a member. An
   * account outside it is refused, because creating a card for one would be
   * handing its GPU access to whoever the caller names.
   */
  bool is_session_account(uid_t uid) {
    const auto *account = ::getpwuid(uid);
    if (account == nullptr) {
      return false;
    }
    const auto *group = ::getgrnam(session_group);
    if (group == nullptr) {
      return false;
    }
    if (account->pw_gid == group->gr_gid) {
      return true;
    }
    for (char **member = group->gr_mem; *member != nullptr; ++member) {
      if (std::strcmp(*member, account->pw_name) == 0) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> cards_of(const options_t &options, uid_t uid) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(options.configfs_root, error)) {
      const auto name = entry.path().filename().string();
      if (entry.is_directory(error) && owned_by(name, uid)) {
        names.emplace_back(name);
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  bool remove_card(const options_t &options, const std::string &name) {
    // configfs removal is an rmdir; the driver unplugs the card underneath it,
    // so a compositor still holding it sees ENODEV rather than a device torn
    // out from under it.
    std::error_code error;
    fs::remove(fs::path {options.configfs_root} / name, error);
    return !error;
  }

  std::string create_card(const options_t &options, uid_t owner, uid_t access_uid) {
    const auto used = used_session_indices(options);
    if (!used) {
      return "ERR create could not read the session seats in use from sysfs";
    }
    unsigned int session_index = 0;
    for (unsigned int candidate = 1; candidate <= max_session_index; ++candidate) {
      if (!(*used)[candidate]) {
        session_index = candidate;
        break;
      }
    }
    if (!session_index) {
      return "ERR exhausted every private session seat is in use";
    }

    // The name keeps the creator's uid, which is what REMOVE and SWEEP are
    // keyed on; the access_uid is whose the card actually is - the caller's
    // own, or the session account that will run a compositor on it.
    const auto name = card_name(owner, session_index);
    const auto dir = fs::path {options.configfs_root} / name;
    std::error_code error;
    if (!fs::create_directory(dir, error)) {
      // error.message() may be empty for a configfs failure, and an empty
      // reason is a reply the caller cannot act on; errno names the truth
      // where the error code does not.
      const auto reason = error ? error.message() : std::string {std::strerror(errno)};
      return "ERR create could not create the card: " + (reason.empty() ? "unknown error" : reason);
    }

    // Everything an identity needs is settled before the card exists: these are
    // writable only while it is disabled, because the KMS object graph is built
    // once at probe.
    const bool configured = write_file(dir / "outputs", "1\n") &&
                            write_file(dir / "role", "session\n") &&
                            write_file(dir / "session_index", std::to_string(session_index) + "\n") &&
                            write_file(dir / "access_uid", std::to_string(access_uid) + "\n") &&
                            write_file(dir / "enabled", "1\n");
    if (!configured) {
      const auto reason = std::string {std::strerror(errno)};
      remove_card(options, name);
      return "ERR create could not configure the card: " + reason;
    }

    const auto card = read_file(dir / "card");
    const auto render_node = read_file(dir / "render_node");
    if (!card || !render_node || card->empty() || render_node->empty()) {
      remove_card(options, name);
      return "ERR create the card did not report its device nodes";
    }

    // The node names come from the driver, not from udev, so there is no wait
    // for a rule to run here. The consumer still has to wait for 92-hermes-kms-
    // access.rules to hand it the render node, which is its own business.
    return "OK " + name + " /dev/dri/" + *card + " /dev/dri/" + *render_node;
  }

  std::string handle_request(const options_t &options, uid_t uid, const std::string &request) {
    if (request == "LIST") {
      std::string reply = "OK";
      for (const auto &name : cards_of(options, uid)) {
        reply += ' ' + name;
      }
      return reply;
    }

    if (request == "SWEEP") {
      unsigned int removed = 0;
      for (const auto &name : cards_of(options, uid)) {
        removed += remove_card(options, name) ? 1 : 0;
      }
      return "OK " + std::to_string(removed);
    }

    if (request == "CREATE" || request.starts_with("CREATE ")) {
      // A bare CREATE is the caller's own card, the way it has always been.
      // `CREATE <uid>` hands the card to a Hermes session account instead,
      // which is what an isolated session needs: its compositor runs as that
      // account, and the card's device nodes go to its access_uid.
      uid_t access_uid = uid;
      if (request != "CREATE") {
        const auto argument = trim(request.substr(7));
        if (argument.empty() || argument.size() > 10 ||
            !std::ranges::all_of(argument, [](unsigned char ch) {
              return std::isdigit(ch);
            })) {
          return "ERR request CREATE takes a numeric uid";
        }
        errno = 0;
        const auto value = std::strtoul(argument.c_str(), nullptr, 10);
        if (errno == ERANGE || value == 0 || value > 0xFFFFFFFFUL) {
          return "ERR request no account has that uid";
        }
        if (!is_session_account(static_cast<uid_t>(value))) {
          return "ERR denied that uid is not a Hermes session account";
        }
        access_uid = static_cast<uid_t>(value);
      }
      if (cards_of(options, uid).size() >= options.max_cards_per_uid) {
        return "ERR limit this user already holds the maximum number of cards";
      }
      return create_card(options, uid, access_uid);
    }

    constexpr std::string_view remove_prefix = "REMOVE ";
    if (request.rfind(remove_prefix, 0) == 0) {
      const auto name = trim(request.substr(remove_prefix.size()));
      if (!owned_by(name, uid)) {
        // Refusing by name rather than by lookup keeps a probe for which cards
        // exist from being answered at all.
        return "ERR denied that card does not belong to this user";
      }
      return remove_card(options, name) ? "OK" : "ERR remove the card could not be removed";
    }

    return "ERR request unknown request";
  }

  std::optional<uid_t> peer_uid(int fd) {
    ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
      return std::nullopt;
    }
    return credentials.uid;
  }

  /**
   * @brief Whether `uid` may ask for cards, per the root-owned allow file.
   *
   * One name or numeric uid per line, `#` starting a comment. Read per request
   * rather than cached, so granting or withdrawing access takes effect without
   * restarting anything.
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
    const auto deadline = std::chrono::steady_clock::now() + socket_timeout;
    std::string request;
    std::array<char, 128> buffer {};
    while (request.size() < max_request_bytes) {
      if (!wait_ready(fd, POLLIN, deadline)) {
        std::fprintf(stderr, "a connection sent no request within %lld seconds; dropping it\n",
                     static_cast<long long>(socket_timeout.count()));
        return std::nullopt;
      }
      const ssize_t count = ::read(fd, buffer.data(), buffer.size());
      if (count <= 0) {
        return count == 0 && !request.empty() ? std::optional {trim(request)} : std::nullopt;
      }
      request.append(buffer.data(), static_cast<size_t>(count));
      if (const auto newline = request.find('\n'); newline != std::string::npos) {
        return trim(request.substr(0, newline));
      }
    }
    return std::nullopt;
  }

  void write_reply(int fd, const std::string &reply) {
    const auto line = reply + "\n";
    const auto deadline = std::chrono::steady_clock::now() + socket_timeout;
    size_t offset = 0;
    while (offset < line.size()) {
      // A peer that stops reading must not hold the broker open either; the
      // same deadline guards this half of the conversation.
      if (!wait_ready(fd, POLLOUT, deadline)) {
        return;
      }
      const ssize_t count = ::write(fd, line.data() + offset, line.size() - offset);
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
      write_reply(connection, "ERR denied this user may not manage Hermes-KMS cards");
      return;
    }

    const auto request = read_request(connection);
    if (!request) {
      write_reply(connection, "ERR request no request was received");
      return;
    }

    // Index allocation reads what exists and then creates, so two clients
    // arriving together could otherwise pick the same free seat.
    const int lock_fd = ::open(options.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0) {
      ::flock(lock_fd, LOCK_EX);
    }
    const auto reply = handle_request(options, *uid, *request);
    if (lock_fd >= 0) {
      ::close(lock_fd);  // Releases the lock.
    }

    write_reply(connection, reply);
  }

  /**
   * @brief The listening socket, from systemd's socket activation when it is
   *        there and bound here when it is not (which is how the tests run it).
   */
  int listening_socket(const options_t &options) {
    const char *listen_pid = ::getenv("LISTEN_PID");
    const char *listen_fds = ::getenv("LISTEN_FDS");
    if (listen_pid && listen_fds && std::strtol(listen_pid, nullptr, 10) == ::getpid() &&
        std::strtol(listen_fds, nullptr, 10) >= 1) {
      return SD_LISTEN_FDS_START;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (options.socket_path.size() >= sizeof(address.sun_path)) {
      std::fprintf(stderr, "socket path is too long: %s\n", options.socket_path.c_str());
      return -1;
    }
    std::memcpy(address.sun_path, options.socket_path.c_str(), options.socket_path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      std::perror("socket");
      return -1;
    }
    ::unlink(options.socket_path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 8) != 0) {
      std::perror("bind");
      ::close(fd);
      return -1;
    }
    return fd;
  }

  void print_usage() {
    std::puts(
      "usage: hermes-kms-card-broker [options]\n"
      "\n"
      "Create and remove Hermes-KMS cards for an unprivileged Hermes.\n"
      "\n"
      "  --socket PATH        listen here when not socket-activated\n"
      "  --allow-file PATH    uids and user names permitted to ask\n"
      "  --configfs PATH      the driver's configfs group\n"
      "  --platform PATH      sysfs platform devices, read to allocate seats\n"
      "  --lock-file PATH     serialises seat allocation between clients\n"
      "  --max-cards N        cards one user may hold at once\n"
      "  --help               this text");
  }

}  // namespace

int main(int argc, char *argv[]) {
  options_t options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument {argv[index]};
    const auto value = [&]() -> std::string {
      return index + 1 < argc ? argv[++index] : "";
    };
    if (argument == "--socket") {
      options.socket_path = value();
    } else if (argument == "--allow-file") {
      options.allow_file = value();
    } else if (argument == "--configfs") {
      options.configfs_root = value();
    } else if (argument == "--platform") {
      options.platform_root = value();
    } else if (argument == "--lock-file") {
      options.lock_file = value();
    } else if (argument == "--max-cards") {
      options.max_cards_per_uid = static_cast<unsigned int>(std::strtoul(value().c_str(), nullptr, 10));
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
      return 2;
    }
  }

  if (!fs::exists(options.configfs_root)) {
    std::fprintf(stderr,
                 "%s does not exist: load the hermes_kms module and mount configfs "
                 "(a driver older than UAPI 12 has no configfs group at all)\n",
                 options.configfs_root.c_str());
    return 1;
  }

  struct sigaction action {};
  action.sa_handler = stop_handler;
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  const int listener = listening_socket(options);
  if (listener < 0) {
    return 1;
  }

  while (!stopping) {
    const int connection = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("accept");
      break;
    }
    serve(options, connection);
    ::close(connection);
  }

  return 0;
}
