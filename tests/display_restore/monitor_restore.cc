/** Production restoration decisions and real subprocess cleanup without a GPU. */
#include "src/platform/linux/bounded_command.h"
#include "src/platform/linux/monitor_restore.h"
#include "src/platform/linux/process_group.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using VDISPLAY::kscreen_output_t;
  using VDISPLAY::monitor_restore::snapshot_t;
  using VDISPLAY::monitor_restore::targets_t;

  void check(bool condition, const char *message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct compositor_t {
    snapshot_t state {std::vector<kscreen_output_t> {}};
    int queries {};
    int commands {};
    bool apply_outputs {true};
    bool reported_success {true};
    targets_t requested;

    bool restore(const targets_t &saved) {
      return VDISPLAY::monitor_restore::restore(saved, [&]() {
        ++queries;
        return state;
      }, [&](const auto &targets) {
        ++commands;
        requested = targets;
        if (apply_outputs) {
          for (auto &output : *state) {
            if (const auto it = targets.find(output.name); it != targets.end()) {
              output.enabled = true;
              output.priority = it->second;
            }
          }
        }
        return reported_success;
      });
    }
  };

  void restores_connected_monitors() {
    compositor_t compositor;
    compositor.state = std::vector<kscreen_output_t> {
      {"DP-1", true, false, 0}, {"HDMI-A-1", true, false, 0}, {"HERMES-1", true, true, 1},
    };
    check(compositor.restore({{"DP-1", 2}, {"HDMI-A-1", 1}}), "physical outputs were not restored");
    check(compositor.queries == 2, "restoration was not read back");
    check(compositor.commands == 1, "restoration was not one transaction");
    check(compositor.requested == targets_t({{"DP-1", 2}, {"HDMI-A-1", 1}}), "saved priorities changed");
  }

  void rejects_unavailable_snapshot() {
    compositor_t compositor;
    compositor.state = std::nullopt;
    check(!compositor.restore({{"DP-1", 1}}), "an unavailable compositor was treated as headless");
    check(compositor.commands == 0, "a layout was applied without a valid snapshot");
  }

  void does_not_wake_unrelated_outputs() {
    compositor_t compositor;
    compositor.state = std::vector<kscreen_output_t> {
      {"DP-1", true, false, 0}, {"HDMI-A-1", false, false, 0}, {"DP-2", true, false, 0},
    };
    check(compositor.restore({{"DP-1", 1}, {"HDMI-A-1", 2}}), "an unplugged monitor blocked recovery");
    check(compositor.requested == targets_t({{"DP-1", 1}}), "restoration touched unrelated/unplugged outputs");
    check(!(*compositor.state)[2].enabled, "a previously disabled monitor was woken");
  }

  void does_not_repeat_a_restored_layout() {
    compositor_t compositor;
    compositor.state = std::vector<kscreen_output_t> {{"DP-1", true, true, 1}};
    check(compositor.restore({{"DP-1", 1}}), "an already active monitor failed verification");
    check(compositor.commands == 0, "an already restored layout caused another modeset");
  }

  void a_successful_command_is_not_confirmation() {
    compositor_t compositor;
    compositor.state = std::vector<kscreen_output_t> {{"DP-1", true, false, 0}};
    compositor.apply_outputs = false;
    const targets_t saved {{"DP-1", 1}};
    check(!compositor.restore(saved), "an unapplied command allowed the virtual output to be released");
    compositor.apply_outputs = true;
    check(compositor.restore(saved), "a subsequent recovery attempt could not succeed");
    check(compositor.commands == 2, "a failed recovery was not retried");
  }

  void verifies_even_after_a_command_timeout() {
    compositor_t compositor;
    compositor.state = std::vector<kscreen_output_t> {{"DP-1", true, false, 0}};
    compositor.reported_success = false;
    check(compositor.restore({{"DP-1", 1}}), "a confirmed transaction was rejected after a command error");
    check(compositor.queries == 2, "an errored command skipped readback");
  }

  void rejects_partial_and_unreadable_results() {
    const targets_t saved {{"DP-1", 1}, {"DP-2", 2}};
    const snapshot_t before {std::vector<kscreen_output_t> {
      {"DP-1", true, false, 0}, {"DP-2", true, false, 0},
    }};
    int queries = 0;
    check(!VDISPLAY::monitor_restore::restore(saved, [&]() -> snapshot_t {
      if (++queries == 1) {
        return before;
      }
      return std::vector<kscreen_output_t> {{"DP-1", true, true, 1}, {"DP-2", true, false, 0}};
    }, [](const auto &) { return true; }), "a partially restored layout was accepted");
    queries = 0;
    check(!VDISPLAY::monitor_restore::restore(saved, [&]() -> snapshot_t {
      return ++queries == 1 ? before : std::nullopt;
    }, [](const auto &) { return true; }), "unreadable verification allowed removal");
  }

  void preserves_headless_and_detached_behavior() {
    compositor_t compositor;
    compositor.state = std::nullopt;
    check(compositor.restore({}), "an empty recovery plan queried the host");
    check(compositor.queries == 0, "a detached/headless session touched host output state");
    compositor.state = std::vector<kscreen_output_t> {{"HERMES-1", true, true, 1}};
    check(compositor.restore({{"DP-1", 1}}), "a genuinely unplugged monitor blocked headless teardown");
    check(compositor.commands == 0, "headless teardown attempted to enable a missing monitor");
  }

  void captures_real_command_output_and_exit_status() {
    bool timed_out = true;
    const auto output = platf::bounded_command::output("printf 'physical output ready'", 1s, &timed_out);
    check(output == std::optional<std::string> {"physical output ready"}, "command output was lost");
    check(!timed_out, "a successful command retained the timeout flag");
    check(!platf::bounded_command::output("printf 'partial'; exit 7", 1s, nullptr), "a failed helper was successful");
  }

  void command_deadlines_are_bounded() {
    for (const auto *command : {"while :; do :; done", "exec 1>&-; while :; do :; done"}) {
      bool timed_out = false;
      const auto started = std::chrono::steady_clock::now();
      check(!platf::bounded_command::output(command, 50ms, &timed_out), "a stuck helper returned success");
      check(timed_out, "a stuck helper did not report its timeout");
      check(std::chrono::steady_clock::now() - started < 1s, "helper cleanup exceeded its bounded deadline");
    }
    int status;
    check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "helper cleanup left an unreaped child");
  }

  void child_waits_do_not_block_cleanup() {
    int ready[2];
    check(pipe(ready) == 0, "could not create readiness pipe");
    const auto child = fork();
    check(child >= 0, "could not fork helper");
    if (child == 0) {
      close(ready[0]);
      if (setpgid(0, 0) != 0) {
        _exit(1);
      }
      ::signal(SIGTERM, SIG_IGN);
      if (write(ready[1], "R", 1) != 1) {
        _exit(2);
      }
      close(ready[1]);
      for (;;) {
        pause();
      }
    }
    close(ready[1]);
    pollfd event {ready[0], POLLIN, 0};
    const bool started = poll(&event, 1, 1000) == 1;
    close(ready[0]);
    // Do not throw until this real workload has been killed and reaped.
    platf::process_group::signal(child, SIGTERM);
    const auto begin = std::chrono::steady_clock::now();
    const bool finished = platf::process_group::wait_for_exit([&]() {
      return platf::process_group::running(child);
    }, 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    check(started, "helper never reached its signal handler");
    check(!finished && elapsed < 1s, "a SIGTERM-resistant child blocked cleanup");
    check(platf::process_group::wait_for_exit([]() { return false; }, 0ms), "an exited child required a grace period");
  }
}

int main() {
  int passed = 0;
  try {
    for (const auto test : {
           restores_connected_monitors, rejects_unavailable_snapshot, does_not_wake_unrelated_outputs,
           does_not_repeat_a_restored_layout, a_successful_command_is_not_confirmation,
           verifies_even_after_a_command_timeout, rejects_partial_and_unreadable_results,
           preserves_headless_and_detached_behavior, captures_real_command_output_and_exit_status,
           command_deadlines_are_bounded, child_waits_do_not_block_cleanup,
         }) {
      test();
      ++passed;
    }
  } catch (const std::exception &error) {
    std::cerr << "After " << passed << " cases: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << passed << " monitor restoration and process cleanup cases passed\n";
}
