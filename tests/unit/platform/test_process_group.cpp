/**
 * @file tests/unit/platform/test_process_group.cpp
 * @brief Real Linux process trees must be cleaned after their launcher exits.
 */
#ifdef __linux__
#include <gtest/gtest.h>
#include <src/platform/linux/process_group.h>
#include <src/platform/linux/service_lifecycle.h>
#include <chrono>
#include <poll.h>
#include <sys/prctl.h>
#include <thread>

namespace {
  using namespace std::chrono_literals;

  bool read_exact(int fd, void *data, size_t size) {
    auto *bytes = static_cast<char *>(data);
    while (size) {
      pollfd event {fd, POLLIN, 0};
      if (poll(&event, 1, 3000) <= 0) {
        return false;
      }
      const auto count = read(fd, bytes, size);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      bytes += count;
      size -= count;
    }
    return true;
  }

  // The supervisor adopts and reaps the orphaned worker. The test process has
  // no direct children in the workload's group, exactly where waitpid(-pgid)
  // incorrectly reported ECHILD in the old liveness check. No orphan is left
  // for the runner's PID 1 to reap, including on assertion failure.
  struct descendant_group_t {
    pid_t supervisor {-1};
    pid_t group {-1};
    int control {-1};

    explicit descendant_group_t(bool ignore_term = false) {
      int commands[2], reports[2], ready[2];
      if (pipe(commands) || pipe(reports) || pipe(ready)) {
        return;
      }
      supervisor = fork();
      if (supervisor == 0) {
        close(commands[1]);
        close(reports[0]);
        if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
          _exit(10);
        }
        const auto leader = fork();
        if (leader == 0) {
          close(ready[0]);
          if (setpgid(0, 0) < 0) {
            _exit(11);
          }
          const auto worker = fork();
          if (worker < 0) {
            _exit(12);
          }
          if (worker == 0) {
            if (ignore_term) {
              ::signal(SIGTERM, SIG_IGN);
            }
            const char started = 'R';
            if (write(ready[1], &started, 1) != 1) {
              _exit(13);
            }
            close(ready[1]);
            for (;;) {
              pause();
            }
          }
          _exit(0);
        }
        if (leader < 0) {
          _exit(14);
        }
        close(ready[1]);
        int status {};
        while (waitpid(leader, &status, 0) < 0 && errno == EINTR) {}
        char started;
        if (!read_exact(ready[0], &started, 1)) {
          kill(-leader, SIGKILL);
          _exit(15);
        }
        close(ready[0]);
        if (write(reports[1], &leader, sizeof(leader)) != sizeof(leader)) {
          kill(-leader, SIGKILL);
          _exit(16);
        }
        close(reports[1]);
        pollfd event {commands[0], POLLIN | POLLHUP, 0};
        while (poll(&event, 1, 10) == 0) {
          while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        }
        kill(-leader, SIGKILL);
        while (waitpid(-1, nullptr, 0) > 0 || errno == EINTR) {}
        _exit(0);
      }
      close(commands[0]);
      close(reports[1]);
      close(ready[0]);
      close(ready[1]);
      control = commands[1];
      if (!read_exact(reports[0], &group, sizeof(group))) {
        group = -1;
      }
      close(reports[0]);
    }

    ~descendant_group_t() {
      if (control >= 0) {
        close(control); // EOF asks the supervisor to kill and reap the workload.
      }
      if (supervisor > 0) {
        while (waitpid(supervisor, nullptr, 0) < 0 && errno == EINTR) {}
      }
    }

    bool wait_for_exit() const {
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (platf::process_group::running(group) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
      }
      return !platf::process_group::running(group);
    }
  };
}

TEST(ProcessGroupTest, FindsDescendantsAfterOriginalLauncherWasReaped) {
  descendant_group_t workload;
  ASSERT_GT(workload.group, 1);
  errno = 0;
  EXPECT_EQ(waitpid(-workload.group, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(platf::process_group::running(workload.group));
  EXPECT_TRUE(platf::process_group::signal(workload.group, SIGTERM));
  EXPECT_TRUE(workload.wait_for_exit());
}

TEST(ProcessGroupTest, EscalatesForDescendantIgnoringSigterm) {
  descendant_group_t workload(true);
  ASSERT_GT(workload.group, 1);
  EXPECT_TRUE(platf::process_group::signal(workload.group, SIGTERM));
  EXPECT_TRUE(platf::process_group::running(workload.group));
  EXPECT_TRUE(platf::process_group::signal(workload.group, SIGKILL));
  EXPECT_TRUE(workload.wait_for_exit());
}

TEST(ProcessGroupTest, LivenessDoesNotConsumeChildExitStatus) {
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    setpgid(0, 0);
    _exit(42);
  }
  siginfo_t info {};
  ASSERT_EQ(waitid(P_PID, child, &info, WEXITED | WNOWAIT), 0);
  EXPECT_TRUE(platf::process_group::running(child));
  int status {};
  EXPECT_EQ(waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 42);
  EXPECT_FALSE(platf::process_group::running(child));
}

TEST(ProcessGroupTest, ReapsTerminatedDirectChildrenDuringCleanup) {
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    setpgid(0, 0);
    _exit(0);
  }
  siginfo_t info {};
  ASSERT_EQ(waitid(P_PID, child, &info, WEXITED | WNOWAIT), 0);
  platf::process_group::reap_children(child);
  EXPECT_FALSE(platf::process_group::running(child));
  EXPECT_EQ(waitpid(child, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

TEST(ProcessGroupTest, RejectsBroadcastAndOwnGroupHandles) {
  for (const auto handle : {std::uintptr_t {0}, std::uintptr_t {1},
                            static_cast<std::uintptr_t>(getpgrp()), std::uintptr_t(-1)}) {
    EXPECT_FALSE(platf::process_group::running(handle));
    EXPECT_FALSE(platf::process_group::signal(handle, SIGTERM));
    EXPECT_EQ(errno, EINVAL);
  }
}

TEST(ServiceLifecycleTest, RecognizesOnlyInstalledHermesUserService) {
  EXPECT_TRUE(platf::is_hermes_user_service_cgroup(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/hermes.service"));
  EXPECT_TRUE(platf::is_hermes_user_service_cgroup(
    "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/hermes.service"));
  for (const auto entry : {"0::/system.slice/hermes.service",
                           "0::/user.slice/user@1000.service/app.slice/konsole.service",
                           "0::/user.slice/user@1000.service/hermes.service/child",
                           "0::/user.slice/user@1000.service/other-hermes.service",
                           "0::/", "", "hermes.service"}) {
    EXPECT_FALSE(platf::is_hermes_user_service_cgroup(entry)) << entry;
  }
}
#endif
