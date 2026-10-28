/**
 * @file tests/unit/test_clipboard_process.cpp
 * @brief Clipboard helpers must not inherit the host's streaming descriptors.
 */
#ifdef __linux__
  #include <array>
  #include <gtest/gtest.h>
  #include <src/platform/linux/clipboard_process.h>
  #include <string>
  #include <sys/socket.h>

namespace {
  struct inherited_sockets_t {
    std::array<int, 3> sockets {-1, -1, -1};

    bool open() {
      for (int &fd : sockets) {
        const int original = socket(AF_INET, SOCK_DGRAM, 0);
        if (original < 0) {
          return false;
        }
        // Deliberately inheritable descriptors, above the shell's own fds.
        fd = fcntl(original, F_DUPFD, 100);
        close(original);
        if (fd < 0) {
          return false;
        }
      }
      return true;
    }

    ~inherited_sockets_t() {
      for (int fd : sockets) {
        if (fd >= 0) {
          close(fd);
        }
      }
    }

    std::string check_command() const {
      std::string command;
      for (int fd : sockets) {
        command += "if [ -e /proc/self/fd/" + std::to_string(fd) + " ]; then exit 47; fi; ";
      }
      return command;
    }
  };
}  // namespace

TEST(ClipboardProcessTest, WriterClosesAllThreeInheritedSocketsBeforeExec) {
  inherited_sockets_t sockets;
  ASSERT_TRUE(sockets.open());
  const auto command = sockets.check_command() + "cat >/dev/null";
  const char *arguments[] = {"sh", "-c", command.c_str(), nullptr};
  platf::clipboard::pipe_t helper("/bin/sh", arguments, true);
  ASSERT_NE(helper.get(), nullptr);
  EXPECT_TRUE(helper.write("clipboard text"));
  for (int fd : sockets.sockets) {
    EXPECT_NE(fcntl(fd, F_GETFD), -1);  // Parent ownership is preserved.
  }
}

TEST(ClipboardProcessTest, ReaderClosesAllThreeInheritedSocketsBeforeExec) {
  inherited_sockets_t sockets;
  ASSERT_TRUE(sockets.open());
  const auto command = sockets.check_command() + "printf isolated";
  const char *arguments[] = {"sh", "-c", command.c_str(), nullptr};
  platf::clipboard::pipe_t helper("/bin/sh", arguments, false);
  ASSERT_NE(helper.get(), nullptr);
  char content[32] {};
  const auto bytes = fread(content, 1, sizeof(content), helper.get());
  EXPECT_EQ(std::string(content, bytes), "isolated");
  EXPECT_TRUE(helper.finish());
}

TEST(ClipboardProcessTest, WritesTheEntireClipboardLimit) {
  const char *arguments[] = {"sh", "-c", "[ \"$(wc -c)\" = 65536 ]", nullptr};
  platf::clipboard::pipe_t helper("/bin/sh", arguments, true);
  ASSERT_NE(helper.get(), nullptr);
  EXPECT_TRUE(helper.write(std::string(64 * 1024, 'x')));
}

TEST(ClipboardProcessTest, AcceptsAnEmptyClipboard) {
  const char *arguments[] = {"sh", "-c", "[ \"$(wc -c)\" = 0 ]", nullptr};
  platf::clipboard::pipe_t helper("/bin/sh", arguments, true);
  EXPECT_TRUE(helper.write(""));
}

TEST(ClipboardProcessTest, FailedWriterDoesNotRaiseSigpipeOrChangeTheSignalMask) {
  sigset_t previous;
  ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &previous), 0);
  const char *arguments[] = {"false", nullptr};
  platf::clipboard::pipe_t helper("/bin/false", arguments, true);
  ASSERT_NE(helper.get(), nullptr);
  EXPECT_FALSE(helper.write(std::string(64 * 1024, 'x')));
  sigset_t current;
  ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &current), 0);
  EXPECT_EQ(sigismember(&current, SIGPIPE), sigismember(&previous, SIGPIPE));
}

TEST(ClipboardProcessTest, ReportsAnExecutableThatCannotBeStarted) {
  const char *arguments[] = {"does-not-exist", nullptr};
  platf::clipboard::pipe_t helper("/hermes-test/does-not-exist", arguments, true);
  EXPECT_FALSE(helper.write("clipboard text"));
}

TEST(ClipboardProcessTest, ReportsAReaderThatFails) {
  const char *arguments[] = {"false", nullptr};
  platf::clipboard::pipe_t helper("/bin/false", arguments, false);
  ASSERT_NE(helper.get(), nullptr);
  EXPECT_EQ(fgetc(helper.get()), EOF);
  EXPECT_FALSE(helper.finish());
}
#endif
