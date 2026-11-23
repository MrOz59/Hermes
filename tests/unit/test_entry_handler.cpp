/**
 * @file tests/unit/test_entry_handler.cpp
 * @brief Test src/entry_handler.*.
 */
#include "../tests_environment.h"
#include "../tests_log_checker.h"

#include <src/entry_handler.h>

TEST(EntryHandlerTests, LogPublisherDataTest) {
  // call log_publisher_data
  log_publisher_data();

  const auto log_file = SunshineEnvironment::log_path();
  ASSERT_TRUE(log_checker::line_starts_with(log_file, "Info: Package Publisher: "));
  ASSERT_TRUE(log_checker::line_starts_with(log_file, "Info: Publisher Website: "));
  ASSERT_TRUE(log_checker::line_starts_with(log_file, "Info: Get support: "));
}
