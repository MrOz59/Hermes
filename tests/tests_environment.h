/**
 * @file tests/tests_environment.h
 * @brief Declarations for SunshineEnvironment.
 */
#pragma once
#include "tests_common.h"

#include <cstdlib>
#include <filesystem>

struct SunshineEnvironment: testing::Environment {
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "hermes-tests-XXXXXX").string();
    const auto *directory = ::mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    artifact_directory = directory;

    mail::man = std::make_shared<safe::mail_raw_t>();
    deinit_log = logging::init(0, log_path());
  }

  void TearDown() override {
    deinit_log = {};
    mail::man = {};
    if (!artifact_directory.empty()) {
      std::error_code error;
      std::filesystem::remove_all(artifact_directory, error);
      EXPECT_FALSE(error) << error.message();
      artifact_directory.clear();
    }
  }

  static std::string log_path() {
    return (artifact_directory / "hermes-test.log").string();
  }

  inline static std::filesystem::path artifact_directory;
  std::unique_ptr<logging::deinit_t> deinit_log;
};
