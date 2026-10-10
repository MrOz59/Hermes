/**
 * @file src/platform/linux/session_attach.cpp
 * @brief Definitions for attaching a Hermes started before login to the session.
 */
// standard includes
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>

// lib includes
#include <unistd.h>

// local includes
#include "session_attach.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/rtsp.h"
#include "virtual_display.h"

using namespace std::literals;

extern char **environ;

namespace platf::session_attach {

  namespace {

    /** What decides which display server and desktop a process belongs to. */
    constexpr std::array display_variables {
      "WAYLAND_DISPLAY"sv,
      "DISPLAY"sv,
      "XDG_CURRENT_DESKTOP"sv,
      "XDG_SESSION_TYPE"sv,
    };

    constexpr auto poll_interval = 5s;

    /**
     * How many polls in a row must find no client before the re-exec. A launch
     * is not a session yet for the second or so its RTSP handshake takes, and
     * a client that just left often comes straight back.
     */
    constexpr int idle_polls_required = 3;

    std::vector<std::string> pending_restart_environment;

    class watcher_t: public platf::deinit_t {
    public:
      watcher_t():
          thread {[this]() {
            run();
          }} {
      }

      ~watcher_t() override {
        {
          std::lock_guard lock {mutex};
          stopping = true;
        }
        wake.notify_all();
        if (thread.joinable()) {
          thread.join();
        }
      }

    private:
      /** @return false once the watch has been told to stop. */
      bool sleep() {
        std::unique_lock lock {mutex};
        return !wake.wait_for(lock, poll_interval, [this]() {
          return stopping;
        });
      }

      void run() {
        int idle_polls = 0;
        bool announced = false;
        auto launches_seen = nvhttp::launch_count();

        while (sleep()) {
          // logind first: it is a file read, and it is "no" for as long as
          // nobody has logged in, which keeps the bus out of the idle case.
          if (!VDISPLAY::graphicalSessionPresent().value_or(false)) {
            idle_polls = 0;
            continue;
          }
          const auto published = VDISPLAY::userManagerEnvironment();
          if (!published) {
            idle_polls = 0;
            continue;
          }
          const auto own = initial_environment();
          const auto session = parse_environment(*published);
          // Published is not the same as running: a desktop that does not
          // clear its variables at logout leaves them behind for good.
          if (!attach_needed(own, session) || !display_server_reachable(session)) {
            idle_polls = 0;
            continue;
          }

          const auto launches = nvhttp::launch_count();
          if (rtsp_stream::session_count() > 0 || launches != launches_seen) {
            launches_seen = launches;
            idle_polls = 0;
            if (!announced) {
              announced = true;
              BOOST_LOG(info) << "[Session] A graphical session has started that this Hermes is not part of. It will "
                                 "restart itself into it once no client is connected; until then it cannot launch "
                                 "applications into the desktop or change its layout."sv;
            }
            continue;
          }
          if (++idle_polls < idle_polls_required) {
            continue;
          }

          BOOST_LOG(info) << "[Session] Restarting into the graphical session that started after Hermes did, with the "
                             "environment the user manager now holds."sv;
          pending_restart_environment = merged_environment(own, session);
          platf::restart();
          return;
        }
      }

      std::mutex mutex;
      std::condition_variable wake;
      bool stopping {false};
      std::thread thread;
    };

  }  // namespace

  environment_t parse_environment(const std::vector<std::string> &assignments) {
    environment_t environment;
    for (const auto &assignment : assignments) {
      const auto equals = assignment.find('=');
      if (equals == std::string::npos || equals == 0) {
        continue;
      }
      environment[assignment.substr(0, equals)] = assignment.substr(equals + 1);
    }
    return environment;
  }

  environment_t parse_environment_block(std::string_view block) {
    std::vector<std::string> assignments;
    while (!block.empty()) {
      const auto end = block.find('\0');
      assignments.emplace_back(block.substr(0, end));
      if (end == std::string_view::npos) {
        break;
      }
      block.remove_prefix(end + 1);
    }
    return parse_environment(assignments);
  }

  bool display_server_reachable(const environment_t &environment) {
    const auto value = [&environment](const char *name) {
      const auto it = environment.find(name);
      return it == environment.end() ? std::string {} : it->second;
    };
    return VDISPLAY::displayServerReachable(value("WAYLAND_DISPLAY"), value("DISPLAY"), value("XDG_RUNTIME_DIR"));
  }

  void drop_stale_display_environment() {
    environment_t live;
    for (const auto name : display_variables) {
      if (const char *value = std::getenv(std::string {name}.c_str()); value && *value) {
        live[std::string {name}] = value;
      }
    }
    if (const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
      live["XDG_RUNTIME_DIR"] = runtime_dir;
    }
    if (!live.contains("WAYLAND_DISPLAY") && !live.contains("DISPLAY")) {
      return;
    }
    if (display_server_reachable(live)) {
      return;
    }

    BOOST_LOG(warning) << "[Session] The environment names a display server that is not running (WAYLAND_DISPLAY="sv
                       << (live.contains("WAYLAND_DISPLAY") ? live["WAYLAND_DISPLAY"] : "") << ", DISPLAY="sv
                       << (live.contains("DISPLAY") ? live["DISPLAY"] : "")
                       << "). These are left over from a session that has ended; starting as if nobody were logged in."sv;
    for (const auto name : display_variables) {
      ::unsetenv(std::string {name}.c_str());
    }
  }

  environment_t initial_environment() {
    std::ifstream initial {"/proc/self/environ", std::ios::binary};
    if (initial) {
      const std::string block {std::istreambuf_iterator<char> {initial}, std::istreambuf_iterator<char> {}};
      if (!block.empty()) {
        return parse_environment_block(block);
      }
    }
    std::vector<std::string> assignments;
    for (char **entry = environ; entry && *entry; ++entry) {
      assignments.emplace_back(*entry);
    }
    return parse_environment(assignments);
  }

  bool attach_needed(const environment_t &own, const environment_t &session) {
    const auto value = [](const environment_t &environment, std::string_view name) {
      const auto it = environment.find(std::string {name});
      return it == environment.end() ? std::string {} : it->second;
    };

    if (value(session, "WAYLAND_DISPLAY"sv).empty() && value(session, "DISPLAY"sv).empty()) {
      return false;
    }
    for (const auto name : display_variables) {
      const auto published = value(session, name);
      if (!published.empty() && published != value(own, name)) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> merged_environment(const environment_t &own, const environment_t &session) {
    auto merged = own;
    for (const auto &[name, value] : session) {
      merged[name] = value;
    }

    std::vector<std::string> assignments;
    assignments.reserve(merged.size());
    for (const auto &[name, value] : merged) {
      assignments.push_back(name + '=' + value);
    }
    return assignments;
  }

  const std::vector<std::string> &restart_environment() {
    return pending_restart_environment;
  }

  std::unique_ptr<platf::deinit_t> start() {
    // A Hermes someone started from a terminal or over SSH has the environment
    // that person chose; replacing it with the desktop's would be a surprise.
    if (VDISPLAY::inLoginSession()) {
      return nullptr;
    }
    // Without logind there is no telling when a session appears.
    if (!VDISPLAY::graphicalSessionPresent()) {
      return nullptr;
    }
    return std::make_unique<watcher_t>();
  }

}  // namespace platf::session_attach
