/**
 * @file entry_handler.h
 * @brief Declarations for entry handling functions.
 */
#pragma once

// standard includes
#include <atomic>
#include <string_view>

// local includes
#include "thread_pool.h"
#include "thread_safe.h"

/**
 * @brief Launch the Web UI.
 * @param path Optional path to append to the base URL.
 * @examples
 * launch_ui();
 * launch_ui("/pin");
 * @examples_end
 */
void launch_ui(const std::optional<std::string> &path = std::nullopt);

/**
 * @brief Functions for handling command line arguments.
 */
namespace args {
  /**
   * @brief Reset the user credentials.
   * @param name The name of the program.
   * @param argc The number of arguments.
   * @param argv The arguments.
   * @examples
   * creds("sunshine", 2, {"new_username", "new_password"});
   * @examples_end
   */
  int creds(const char *name, int argc, char *argv[]);

  /**
   * @brief Print help to stdout, then exit.
   * @param name The name of the program.
   * @examples
   * help("sunshine");
   * @examples_end
   */
  int help(const char *name);

  /**
   * @brief Print the version to stdout, then exit.
   * @examples
   * version();
   * @examples_end
   */
  int version();

}  // namespace args

/**
 * @brief Functions for handling the lifetime of Sunshine.
 */
namespace lifetime {
  extern char **argv;
  extern std::atomic_int desired_exit_code;

  /**
   * @brief Terminates Sunshine gracefully with the provided exit code.
   * @param exit_code The exit code to return from main().
   * @param async Specifies whether our termination will be non-blocking.
   */
  void exit_sunshine(int exit_code, bool async);

  /**
   * @brief Breaks into the debugger or terminates Sunshine if no debugger is attached.
   */
  void debug_trap();

  /**
   * @brief Get the argv array passed to main().
   */
  char **get_argv();
}  // namespace lifetime

/**
 * @brief Log the publisher metadata provided from CMake.
 */
void log_publisher_data();
