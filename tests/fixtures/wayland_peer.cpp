// SPDX-License-Identifier: GPL-3.0-only
#include "../helpers/posix_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>

extern char **environ;

namespace {
  using namespace fixture;

  void relay_fixture() {
    constexpr int channel = 3;
    for (const auto &entry : fs::directory_iterator("/proc/self/fd")) {
      const auto descriptor = entry.path().filename();
      require(descriptor == "0" || descriptor == "3", "a caller descriptor survived helper exec");
    }
    for (const char *const *item = environ; *item; ++item) {
      const std::string_view variable {*item};
      for (auto prefix : {"LD_", "WAYLAND_", "PIPEWIRE_", "HERMES_TEST_"}) {
        require(!variable.starts_with(prefix), "a caller environment variable survived helper exec");
      }
    }

    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) < 0) {
      fail_system("socketpair");
    }
    fd client {descriptors[0], "client socket"};
    fd server {descriptors[1], "server socket"};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    char success = 1;
    iovec payload {&success, sizeof(success)};
    msghdr message {};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto *item = CMSG_FIRSTHDR(&message);
    item->cmsg_level = SOL_SOCKET;
    item->cmsg_type = SCM_RIGHTS;
    item->cmsg_len = CMSG_LEN(sizeof(int));
    const auto sent_fd = client.get();
    std::memcpy(CMSG_DATA(item), &sent_fd, sizeof(sent_fd));
    require(::sendmsg(channel, &message, MSG_NOSIGNAL) == 1, "could not relay the fixture socket");
    client.close();

    set_socket_timeout(server.get(), 5s);
    require(receive_exact(server.get(), 5) == "ready", "unexpected handshake from connection probe");
    send_all(server.get(), "ok");
    pollfd events {channel, POLLIN, 0};
    wait_for(std::span {&events, 1}, 10s);
  }

  void expect_clean_peer(const fs::path &proc) {
    constexpr std::array caps {"CapPrm", "CapEff", "CapInh", "CapAmb"};
    std::ifstream status {proc / "status"};
    require(status.good(), "could not inspect helper credentials");
    unsigned seen_caps = 0;
    bool no_new_privileges = false;
    for (std::string line; std::getline(status, line);) {
      const auto split = line.find(':');
      if (split == std::string::npos) {
        continue;
      }
      const auto field = line.substr(0, split);
      std::istringstream value {line.substr(split + 1)};
      if (std::ranges::find(caps, field) != caps.end()) {
        std::uint64_t number;
        require((value >> std::hex >> number) && number == 0, "helper retained a capability");
        ++seen_caps;
      } else if (field == "NoNewPrivs") {
        int number;
        no_new_privileges = (value >> number) && number == 1;
      }
    }
    require(seen_caps == caps.size() && no_new_privileges, "helper must drop capabilities and prohibit new privileges");
  }

  void expect_checked(process &child, std::chrono::seconds timeout) {
    try {
      child.expect_exit(timeout);
    } catch (...) {
      child.show_journal();
      throw;
    }
  }

  void check_wayland_peer(const fs::path &helper_path, const fs::path &probe_path, bool no_listener) {
    temporary_directory root {"hermes-wayland-peer"};
    const auto helper = fs::canonical(helper_path).string();
    const auto probe = fs::canonical(probe_path).string();
    const auto journal = root.path / "probe.log";

    // Deliberately leak a non-CLOEXEC FD and a variable into the caller.
    // The production launcher must prevent both reaching its exec'd helper.
    {
      const auto fixture_path = fs::read_symlink("/proc/self/exe").string();
      fd leak {::open("/dev/null", O_RDONLY), "open caller descriptor"};
      if (::setenv("HERMES_TEST_SECRET", "must-not-reach-helper", 1) < 0) {
        fail_system("set test marker");
      }
      process child {{probe, "relay", fixture_path, (root.path / "unused").string()}, journal};
      ::unsetenv("HERMES_TEST_SECRET");
      expect_checked(child, 10s);
    }
    std::cout << "PASS: production FD handoff, helper lifetime/reaping, descriptor/environment isolation, parent state preserved\n";

    const std::pair<std::string, std::string> failures[] {
      {helper, "relative"},
      {helper, (root.path / "missing").string()},
      {(root.path / "missing-helper").string(), (root.path / "socket").string()},
    };
    for (const auto &[executable, path] : failures) {
      process child {{probe, "failure", executable, path}, journal};
      expect_checked(child, 30s);
    }
    std::cout << "PASS: invalid/missing socket and missing helper fail and reap without host fallback\n";

    if (no_listener) {
      std::cout << "NOT RUN: real AF_UNIX connect and peer executable identity (listener tests disabled)\n";
      return;
    }

    for (const std::string mode : {"direct", "relay"}) {
      const auto path = root.path / mode;
      const auto listener = unix_listener(path, 4);
      process child {{probe, mode, helper, path.string()}, journal};
      try {
        pollfd waiting {listener.get(), POLLIN, 0};
        require(wait_for(std::span {&waiting, 1}, 5s) > 0, "connection probe did not reach the listener");
        fd connection {::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC), "accept fixture connection"};
        set_socket_timeout(connection.get(), 5s);
        require(receive_exact(connection.get(), 5) == "ready", "unexpected handshake from connection probe");
        ucred peer {};
        socklen_t length = sizeof(peer);
        if (::getsockopt(connection.get(), SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0) {
          fail_system("inspect Unix peer credentials");
        }
        require(length == sizeof(peer) && peer.uid == ::getuid() && peer.gid == ::getgid(), "socket peer has the wrong UID or GID");
        const auto proc = fs::path {"/proc"} / std::to_string(peer.pid);
        std::error_code read_error;
        const auto executable = fs::read_symlink(proc / "exe", read_error);
        if (mode == "direct") {
          require(peer.pid == child.pid() && read_error == std::errc::permission_denied, "direct peer identity did not stay non-dumpable");
        } else {
          require(peer.pid != child.pid() && !read_error && executable == helper, "relayed connection did not have helper executable identity");
          expect_clean_peer(proc);
        }
        send_all(connection.get(), "ok");
        connection.close();
        child.expect_exit(10s);
      } catch (...) {
        child.show_journal();
        throw;
      }
      std::cout << "PASS: " << mode << " peer identity; parent permissions unchanged\n";
    }

    const auto path = root.path / "full";
    const auto listener = unix_listener(path, 0);
    const auto queued = unix_connect(path, 5s);
    const auto started = std::chrono::steady_clock::now();
    process child {{probe, "failure", helper, path.string()}, journal};
    expect_checked(child, 30s);
    require(std::chrono::steady_clock::now() - started < 5s, "a full socket backlog did not fail promptly");
    std::cout << "PASS: full socket backlog fails promptly without fallback\n";
  }
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view {argv[1]} == "--connect-wayland") {
      relay_fixture();
      return 0;
    }
    const bool no_listener = argc > 1 && std::string_view {argv[1]} == "--no-listener";
    const int first = no_listener ? 2 : 1;
    if (argc != first + 2) {
      std::cerr << "usage: hermes-wayland-peer-test [--no-listener] DETACHED_SESSION WAYLAND_PEER_CHECK\n";
      return 2;
    }
    check_wayland_peer(argv[first], argv[first + 1], no_listener);
  } catch (const std::exception &error) {
    std::cerr << "Wayland peer fixture: " << error.what() << '\n';
    return 1;
  }
}
