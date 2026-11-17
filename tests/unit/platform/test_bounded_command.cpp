/**
 * @file tests/unit/platform/test_bounded_command.cpp
 * @brief Test that a bounded command keeps its reply while Hermes reaps children.
 */
#include "../../tests_common.h"

#include <src/platform/linux/bounded_command.h>
#include <src/process.h>

#include <atomic>
#include <string>
#include <thread>

using namespace std::chrono_literals;

TEST(BoundedCommand, KeepsItsReplyWhileTheAppReaperRuns) {
  // proc_t::running() reaps every child with waitpid(-1), and it is called
  // from client requests and the stream loop. When that took a command's
  // exit status first, a complete kscreen-doctor reply was thrown away:
  // KWin had the virtual output enabled while Hermes logged that it could
  // not resolve its geometry. Without the fix, 2 to 10 of these 50 runs
  // lost their reply.
  std::atomic<bool> stop {false};
  std::thread reaper([&stop]() {
    while (!stop) {
      proc::proc.running();
      std::this_thread::sleep_for(1ms);
    }
  });

  int lost = 0;
  for (int i = 0; i < 50; ++i) {
    const auto reply = platf::bounded_command::output("printf 'reply'", 2s, nullptr);
    if (!reply || *reply != "reply") {
      ++lost;
    }
  }
  stop = true;
  reaper.join();

  EXPECT_EQ(lost, 0);
}

TEST(BoundedCommand, AFailedCommandStillFails) {
  EXPECT_FALSE(platf::bounded_command::output("printf 'partial'; exit 7", 2s, nullptr));
}
