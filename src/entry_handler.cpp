/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <format>
#include <iostream>
#include <thread>

// local includes
#include "config.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"

using namespace std::literals;

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
    std::raise(SIGTRAP);
    // If debug trap still doesn't work, abort
    abort();
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}
