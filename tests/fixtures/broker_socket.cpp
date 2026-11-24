// SPDX-License-Identifier: GPL-3.0-only
#include "../helpers/posix_fixture.h"

#include <array>
#include <sys/file.h>
#include <sys/stat.h>

namespace {
  using namespace fixture;

  fd request(const fs::path &path, std::string_view command, std::chrono::seconds timeout = 2s) {
    auto peer = unix_connect(path, timeout);
    send_all(peer.get(), command);
    if (::shutdown(peer.get(), SHUT_WR) < 0) {
      fail_system("shutdown");
    }
    return peer;
  }

  std::string reply(const fd &peer) {
    std::string response;
    std::array<char, 4096> buffer;
    while (auto size = receive(peer.get(), buffer)) {
      response.append(buffer.data(), size);
      if (response.contains('\n')) {
        break;
      }
    }
    constexpr std::string_view whitespace = " \t\r\n\v\f";
    const auto start = response.find_first_not_of(whitespace);
    require(start != std::string::npos, "broker closed before replying");
    return response.substr(start, response.find_last_not_of(whitespace) - start + 1);
  }

  void set_lock(const fd &lock, int mode) {
    if (::flock(lock.get(), mode) < 0) {
      fail_system("flock on broker fixture lock");
    }
  }

  void check_session_concurrency(const fs::path &binary) {
    temporary_directory root {"hermes-concurrency"};
    const auto socket_path = root.path / "socket";
    const auto lock_path = root.path / "mutation.lock";
    const auto allow_path = root.path / "allow";
    const auto map_path = root.path / "map";
    {
      std::ofstream allow {allow_path};
      allow << ::getuid() << '\n';
      require(allow.good(), "could not write fixture allow file");
      fd map {::open(map_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600), "create fixture map"};
    }

    auto listener = unix_listener(socket_path, 16);
    // The broker receives its own private socket through the real systemd
    // socket-activation contract. These requests never create users or units.
    process broker {{
      fs::canonical(binary).string(), "--allow-file", allow_path.string(), "--state-file", map_path.string(),
      "--home-root", (root.path / "homes").string(), "--lock-file", lock_path.string(),
    }, root.path / "journal", listener.get()};
    listener.close();

    try {
      require(reply(request(socket_path, "LIST\n")) == "OK", "fixture broker did not start");
      {
        auto slow = unix_connect(socket_path, 2s);
        send_all(slow.get(), "START abcd\nARG /usr/bin/false\n");
        std::this_thread::sleep_for(50ms);
        const auto started = std::chrono::steady_clock::now();
        require(reply(request(socket_path, "LOOKUP dcba\n")).starts_with("ERR notfound"), "LOOKUP did not respond while a request was incomplete");
        require(std::chrono::steady_clock::now() - started < 2s, "LOOKUP waited for an incomplete request");
      }
      std::cout << "PASS: an incomplete request does not block read-only RPCs\n";

      fd lock {::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600), "open fixture mutation lock"};
      set_lock(lock, LOCK_EX);
      auto first = request(socket_path, "STOP abcd\n");
      auto second = request(socket_path, "STOP dcba\n");
      std::array<pollfd, 2> waiting {{{first.get(), POLLIN, 0}, {second.get(), POLLIN, 0}}};
      require(wait_for(waiting, 50ms) == 0, "mutations escaped their lock");
      require(reply(request(socket_path, "LIST\n")) == "OK", "queued mutations blocked liveness traffic");
      set_lock(lock, LOCK_UN);
      require(reply(first).starts_with("ERR notfound"), "first queued mutation did not finish");
      require(reply(second).starts_with("ERR notfound"), "second queued mutation did not finish");
      std::cout << "PASS: mutations queue while read-only RPCs remain available\n";

      if (::fchmod(lock.get(), 0666) < 0) {
        fail_system("make fixture mutation lock deliberately unsafe");
      }
      const auto started = std::chrono::steady_clock::now();
      require(reply(request(socket_path, "STOP abcd\n")).starts_with("ERR busy"), "broker did not reject an unsafe lock");
      require(std::chrono::steady_clock::now() - started < 2s, "an unsafe lock was retried");
      std::cout << "PASS: an unsafe mutation lock fails immediately\n";
    } catch (...) {
      broker.show_journal();
      throw;
    }
  }

  void check_queued_ensure(const fs::path &socket_path, const fs::path &lock_path, const std::string &active, const std::string &next) {
    fd lock {::open(lock_path.c_str(), O_RDWR | O_CLOEXEC), "open broker mutation lock"};
    set_lock(lock, LOCK_EX);
    auto pending = request(socket_path, "ENSURE " + next + '\n', 180s);
    pollfd waiting {pending.get(), POLLIN, 0};
    require(wait_for(std::span {&waiting, 1}, 50ms) == 0, "ENSURE did not queue behind the mutation lock");
    require(reply(request(socket_path, "STATUS " + active + '\n')) == "OK active hermes-session-01.service", "liveness was blocked or lost");
    set_lock(lock, LOCK_UN);
    std::cout << reply(pending) << '\n';
  }
}  // namespace

int main(int argc, char **argv) {
  try {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (argc == 3 && command == "session-concurrency") {
      check_session_concurrency(argv[2]);
    } else if (argc == 4 && command == "request") {
      std::cout << reply(request(argv[2], argv[3], 180s)) << '\n';
    } else if (argc == 6 && command == "queued-ensure") {
      check_queued_ensure(argv[2], argv[3], argv[4], argv[5]);
    } else if (argc == 3 && command == "bind-socket") {
      // Intentionally leave the socket node after closing the listener.
      unix_listener(argv[2], 1);
    } else if (argc == 3 && command == "open-input") {
      const auto input = ::open(argv[2], O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (input < 0 && (errno == EACCES || errno == EPERM || errno == ENOENT)) {
        return 1;
      }
      fd device {input, "open input node"};
    } else {
      std::cerr << "usage: hermes-broker-socket-test session-concurrency BROKER\n"
                   "       hermes-broker-socket-test request SOCKET MESSAGE\n"
                   "       hermes-broker-socket-test queued-ensure SOCKET LOCK ACTIVE_CLIENT NEXT_CLIENT\n"
                   "       hermes-broker-socket-test bind-socket PATH\n"
                   "       hermes-broker-socket-test open-input PATH\n";
      return 2;
    }
  } catch (const std::exception &error) {
    std::cerr << "broker socket fixture: " << error.what() << '\n';
    return 1;
  }
}
