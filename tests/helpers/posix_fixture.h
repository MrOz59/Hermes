// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fixture {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  inline void require(bool condition, std::string_view message) {
    if (!condition) {
      throw std::runtime_error(std::string {message});
    }
  }

  [[noreturn]] inline void fail_system(std::string_view operation) {
    throw std::system_error(errno, std::generic_category(), std::string {operation});
  }

  class fd {
  public:
    fd(int value, std::string_view operation):
        value {value} {
      if (value < 0) {
        fail_system(operation);
      }
    }

    fd(const fd &) = delete;
    fd &operator=(const fd &) = delete;
    fd(fd &&other) noexcept:
        value {std::exchange(other.value, -1)} {
    }

    ~fd() {
      close();
    }

    int get() const noexcept {
      return value;
    }

    void close() noexcept {
      if (value >= 0) {
        ::close(std::exchange(value, -1));
      }
    }

  private:
    int value;
  };

  class temporary_directory {
  public:
    explicit temporary_directory(std::string_view name) {
      auto pattern = (fs::absolute(fs::temp_directory_path()) / (std::string {name} + "-XXXXXX")).string();
      if (!::mkdtemp(pattern.data())) {
        fail_system("mkdtemp");
      }
      path = std::move(pattern);
    }

    temporary_directory(const temporary_directory &) = delete;
    temporary_directory &operator=(const temporary_directory &) = delete;

    ~temporary_directory() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }

    fs::path path;
  };

  inline sockaddr_un unix_address(const fs::path &path) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto &name = path.native();
    require(!name.empty() && name.size() < sizeof(address.sun_path), "Unix socket path is empty or too long");
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    return address;
  }

  inline fd unix_listener(const fs::path &path, int backlog) {
    fd listener {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), "socket"};
    const auto address = unix_address(path);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0 ||
        ::listen(listener.get(), backlog) < 0) {
      fail_system("listen on Unix socket");
    }
    return listener;
  }

  inline int wait_for(std::span<pollfd> descriptors, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      const auto milliseconds = remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;
      const int result = ::poll(descriptors.data(), descriptors.size(), milliseconds);
      if (result >= 0) {
        return result;
      }
      if (errno != EINTR) {
        fail_system("poll");
      }
    }
  }

  inline void set_socket_timeout(int socket, std::chrono::seconds timeout) {
    const timeval limit {timeout.count(), 0};
    for (int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
      if (::setsockopt(socket, SOL_SOCKET, option, &limit, sizeof(limit)) < 0) {
        fail_system("set Unix socket timeout");
      }
    }
  }

  inline fd unix_connect(const fs::path &path, std::chrono::seconds timeout) {
    fd peer {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), "socket"};
    const auto address = unix_address(path);
    set_socket_timeout(peer.get(), timeout);
    if (::connect(peer.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
      fail_system("connect to Unix socket");
    }
    return peer;
  }

  inline void send_all(int peer, std::string_view bytes) {
    while (!bytes.empty()) {
      const auto sent = ::send(peer, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent <= 0) {
        fail_system("send");
      }
      bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
  }

  inline std::size_t receive(int peer, std::span<char> bytes) {
    ssize_t size;
    do {
      size = ::recv(peer, bytes.data(), bytes.size(), 0);
    } while (size < 0 && errno == EINTR);
    if (size < 0) {
      fail_system("recv");
    }
    return static_cast<std::size_t>(size);
  }

  inline std::string receive_exact(int peer, std::size_t length) {
    std::string bytes(length, '\0');
    std::size_t received = 0;
    while (received < length) {
      const auto size = receive(peer, std::span<char> {bytes}.subspan(received));
      require(size != 0, "peer closed before completing the handshake");
      received += size;
    }
    return bytes;
  }

  class process {
  public:
    process(const std::vector<std::string> &arguments, const fs::path &journal, int listener = -1):
        journal {journal} {
      std::vector<char *> argv;
      for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
      }
      argv.push_back(nullptr);

      fd output {::open(journal.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), "open child journal"};
      child = ::fork();
      if (child < 0) {
        fail_system("fork");
      }
      if (child == 0) {
        if (::setsid() < 0 || ::dup2(output.get(), STDOUT_FILENO) < 0 ||
            ::dup2(output.get(), STDERR_FILENO) < 0) {
          ::_exit(126);
        }
        if (listener >= 0) {
          if (::dup2(listener, 3) < 0 || ::fcntl(3, F_SETFD, 0) < 0 ||
              ::setenv("LISTEN_PID", std::to_string(::getpid()).c_str(), 1) < 0 ||
              ::setenv("LISTEN_FDS", "1", 1) < 0) {
            ::_exit(126);
          }
        }
        ::execv(argv.front(), argv.data());
        ::_exit(127);
      }
      original_pid = child;
    }

    process(const process &) = delete;
    process &operator=(const process &) = delete;

    ~process() {
      if (child <= 0) {
        return;
      }
      signal(SIGTERM);
      try {
        if (wait_for_exit(3s)) {
          return;
        }
      } catch (const std::exception &) {
      }
      signal(SIGKILL);
      while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
    }

    pid_t pid() const noexcept {
      return original_pid;
    }

    void expect_exit(std::chrono::milliseconds timeout) {
      require(wait_for_exit(timeout), "child timed out");
      require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited unsuccessfully");
    }

    void show_journal() const {
      std::ifstream stream {journal};
      if (stream) {
        std::cerr << stream.rdbuf();
      }
    }

  private:
    bool wait_for_exit(std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (child > 0) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
          child = -1;
          return true;
        }
        if (result < 0 && errno != EINTR) {
          fail_system("waitpid");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(10ms);
      }
      return true;
    }

    void signal(int number) const noexcept {
      if (::kill(-child, number) < 0 && errno == ESRCH) {
        ::kill(child, number);
      }
    }

    fs::path journal;
    pid_t child {-1};
    pid_t original_pid {-1};
    int status {};
  };
}  // namespace fixture
