/**
 * @file tests/integration/test_external_commands.cpp
 * @brief Integration tests for running external commands with Linux
 */
#include "../tests_common.h"

// standard includes
#include <format>
#include <string>
#include <tuple>
#include <vector>

// lib includes
#include <boost/process/v1.hpp>

// local includes
#include "src/platform/common.h"

// Test data structure for parameterized testing
struct ExternalCommandTestData {
  std::string command;
  bool should_succeed;
  std::string description;
  std::string working_directory;  // Optional: if empty, uses SUNSHINE_SOURCE_DIR

};

class ExternalCommandTest: public ::testing::TestWithParam<ExternalCommandTestData> {
protected:
  // Helper function to run a command using the existing process infrastructure
  static std::pair<int, std::string> runCommand(const std::string &cmd, const std::string_view &working_dir) {
    const auto env = boost::this_process::environment();

    // Determine the working directory: use the provided working_dir or fall back to SUNSHINE_SOURCE_DIR
    boost::filesystem::path effective_working_dir;

    if (!working_dir.empty()) {
      effective_working_dir = working_dir;
    } else {
      // Use SUNSHINE_SOURCE_DIR build definition as the default working directory
      effective_working_dir = SUNSHINE_SOURCE_DIR;
    }

    std::error_code ec;

    // Create a temporary file to capture output
    const auto temp_file = std::tmpfile();
    if (!temp_file) {
      return {-1, "Failed to create temporary file for output"};
    }

    // Run the command using the existing platf::run_command function
    auto child = platf::run_command(
      false,  // not elevated
      false,  // not interactive
      cmd,
      effective_working_dir,
      env,
      temp_file,
      ec,
      nullptr  // no process group
    );

    if (ec) {
      std::fclose(temp_file);
      return {-1, std::format("Failed to start command: {}", ec.message())};
    }

    // Wait for the command to complete
    child.wait();
    int exit_code = child.exit_code();

    // Read the output from the temporary file
    std::rewind(temp_file);
    std::string output;
    std::array<char, 1024> buffer {};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), temp_file)) {
      // std::string constructor automatically handles null-terminated strings
      output += std::string(buffer.data());
    }
    std::fclose(temp_file);

    return {exit_code, output};
  }

};

// Test case implementation
TEST_P(ExternalCommandTest, RunExternalCommand) {
  const auto &[command, should_succeed, description, working_directory] = GetParam();

  BOOST_LOG(info) << "Running external command test: " << description;
  BOOST_LOG(debug) << "Command: " << command;

  auto [exit_code, output] = runCommand(command, working_directory);

  BOOST_LOG(debug) << "Command exit code: " << exit_code;
  if (!output.empty()) {
    BOOST_LOG(debug) << "Command output: " << output;
  }

  if (should_succeed) {
    EXPECT_EQ(exit_code, 0) << "Command should have succeeded\nOutput: " << output;
  } else {
    EXPECT_NE(exit_code, 0) << "Command should have failed\nOutput: " << output;
  }
}

#ifdef UDEVADM_EXECUTABLE
  #define UDEV_TESTS \
    ExternalCommandTestData { \
      std::format("{0} verify {1}/src_assets/linux/misc/60-hermes.rules {1}/src_assets/linux/misc/74-hermes-session-acl.rules", UDEVADM_EXECUTABLE, SUNSHINE_TEST_BIN_DIR), \
      true, \
      "Test udev rules file", \
      "" \
    },
#else
  #define UDEV_TESTS
#endif

// Test data
INSTANTIATE_TEST_SUITE_P(
  ExternalCommands,
  ExternalCommandTest,
  ::testing::Values(
    UDEV_TESTS
    ExternalCommandTestData {"which sh", true, "Simple command test", ""},
    ExternalCommandTestData {
      "non_existent_command_12345",
      false,
      "Test command that should fail",
      ""
    }
  ),
  [](const ::testing::TestParamInfo<ExternalCommandTestData> &info) {
    // Generate test names from a description
    std::string name = info.param.description;
    // Replace spaces and special characters with underscores for valid test names
    std::replace_if(name.begin(), name.end(), [](char c) {
      return !std::isalnum(c);
    },
                    '_');
    return name;
  }
);
