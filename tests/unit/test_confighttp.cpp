/**
 * @file tests/unit/test_confighttp.cpp
 * @brief Test src/confighttp.* helpers.
 */
#include "../tests_common.h"

#include <src/confighttp.h>

// The preflight aggregation reads platform-derived state (encoder probe result,
// virtual-display backend, session environment), so initialize the platform.
struct PreflightTest: PlatformTestSuite {};

TEST_F(PreflightTest, ShapeIsWellFormed) {
  const auto preflight = confighttp::hestia_preflight_json();

  ASSERT_TRUE(preflight.contains("ready"));
  ASSERT_TRUE(preflight["ready"].is_boolean());

  ASSERT_TRUE(preflight.contains("checks"));
  ASSERT_TRUE(preflight["checks"].is_array());
  ASSERT_FALSE(preflight["checks"].empty());

  bool any_fail = false;
  for (const auto &check : preflight["checks"]) {
    ASSERT_TRUE(check.contains("id"));
    ASSERT_TRUE(check["id"].is_string());
    ASSERT_FALSE(check["id"].get<std::string>().empty());

    ASSERT_TRUE(check.contains("status"));
    const auto status = check.value("status", std::string {});
    ASSERT_TRUE(status == "ok" || status == "warn" || status == "fail")
      << "unexpected status: " << status;
    if (status == "fail") {
      any_fail = true;
    }

    ASSERT_TRUE(check.contains("message"));
    ASSERT_TRUE(check["message"].is_string());
    ASSERT_FALSE(check["message"].get<std::string>().empty());
  }

  // `ready` must be the negation of "any check failed".
  ASSERT_EQ(preflight["ready"].get<bool>(), !any_fail);
}
