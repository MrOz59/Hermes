// SPDX-License-Identifier: GPL-3.0-only
/** @file test_session_broker_workers.cpp
 *  @brief Real-process concurrency, limits and ownership for broker workers.
 */
#if defined(__linux__)
  #include "tools/session-broker-workers.h"

  #include <chrono>
  #include <fcntl.h>
  #include <gtest/gtest.h>
  #include <poll.h>
  #include <thread>

namespace {
  using namespace std::chrono_literals;
  using hermes::session_broker::worker_pool;

  class SessionBrokerWorkersTest: public testing::Test {
  protected:
    void SetUp() override {
      ASSERT_EQ(::pipe2(gate, O_CLOEXEC), 0);
      ASSERT_EQ(::pipe2(output, O_CLOEXEC), 0);
    }

    void TearDown() override {
      for (const auto fd : {gate[0], gate[1], output[0], output[1]}) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    }

    static bool exits(worker_pool &workers, pid_t child) {
      for (int attempt = 0; attempt < 100; ++attempt) {
        workers.reap();
        if (!workers.contains(child)) {
          return true;
        }
        std::this_thread::sleep_for(10ms);
      }
      return false;
    }

    int gate[2] {-1, -1}, output[2] {-1, -1};
  };

  TEST_F(SessionBrokerWorkersTest, ABlockedOperationDoesNotBlockAnotherWorker) {
    worker_pool workers {2};
    const auto slow = workers.spawn({gate[1], output[0]}, [&]() {
      char byte;
      if (::read(gate[0], &byte, 1) == 1) {
        ::write(output[1], "s", 1);
      }
    });
    ASSERT_GT(slow, 0);
    const auto fast = workers.spawn({gate[0], gate[1], output[0]}, [&]() {
      ::write(output[1], "f", 1);
    });
    ASSERT_GT(fast, 0);
    pollfd ready {output[0], POLLIN, 0};
    ASSERT_EQ(::poll(&ready, 1, 1000), 1);
    char byte = 0;
    ASSERT_EQ(::read(output[0], &byte, 1), 1);
    EXPECT_EQ(byte, 'f');
    EXPECT_TRUE(workers.contains(slow));
    ASSERT_EQ(::write(gate[1], "x", 1), 1);
    EXPECT_TRUE(exits(workers, slow));
    EXPECT_TRUE(exits(workers, fast));
  }

  TEST_F(SessionBrokerWorkersTest, ExhaustionIsBoundedAndAReapedSlotCanBeReused) {
    worker_pool workers {1};
    const auto first = workers.spawn({gate[1]}, [&]() {
      char byte;
      ::read(gate[0], &byte, 1);
    });
    ASSERT_GT(first, 0);
    EXPECT_EQ(workers.spawn({}, []() {
    }),
              -1);
    ASSERT_EQ(::write(gate[1], "x", 1), 1);
    ASSERT_TRUE(exits(workers, first));
    EXPECT_GT(workers.spawn({}, []() {
    }),
              0);
  }

  TEST_F(SessionBrokerWorkersTest, ReapingDoesNotConsumeAnUnrelatedChild) {
    const auto unrelated = ::fork();
    ASSERT_GE(unrelated, 0);
    if (unrelated == 0) {
      ::_exit(7);
    }
    worker_pool workers {1};
    const auto own = workers.spawn({}, []() {
    });
    ASSERT_GT(own, 0);
    EXPECT_TRUE(exits(workers, own));
    int status = 0;
    EXPECT_EQ(::waitpid(unrelated, &status, 0), unrelated);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 7);
  }

  TEST_F(SessionBrokerWorkersTest, ShutdownReclaimsEvenAWorkerThatIgnoresTerm) {
    pid_t child = -1;
    {
      worker_pool workers {1};
      child = workers.spawn({output[0]}, [&]() {
        ::signal(SIGTERM, SIG_IGN);
        ::write(output[1], "r", 1);
        char byte;
        ::read(gate[0], &byte, 1);
      });
      ASSERT_GT(child, 0);
      pollfd ready {output[0], POLLIN, 0};
      ASSERT_EQ(::poll(&ready, 1, 1000), 1);
    }
    errno = 0;
    EXPECT_EQ(::waitpid(child, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
  }
}  // namespace
#endif
