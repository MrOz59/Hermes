/**
 * @file tests/unit/test_detached_scope.cpp
 * @brief Detached commands run in transient scopes Hermes can stop.
 */
#include "../tests_common.h"

#include <src/platform/linux/detached_scope.h>

using namespace std::chrono_literals;
namespace scope = platf::detached_scope;

TEST(DetachedScope, UnitNamesAreUniquePerLaunchAndCommand) {
  EXPECT_EQ(scope::unit_name(4242, 7, 0), "hermes-app-4242-7-0.scope");
  EXPECT_NE(scope::unit_name(4242, 7, 0), scope::unit_name(4242, 7, 1));
  EXPECT_NE(scope::unit_name(4242, 7, 0), scope::unit_name(4242, 8, 0));
  EXPECT_NE(scope::unit_name(4242, 7, 0), scope::unit_name(4243, 7, 0));
}

TEST(DetachedScope, WrapsTheCommandUnchanged) {
  EXPECT_EQ(
    scope::wrap("/usr/bin/systemd-run", "hermes-app-1-2-0.scope", 15s, "setsid steam steam://open/bigpicture"),
    "/usr/bin/systemd-run --user --scope --quiet --collect --unit=hermes-app-1-2-0.scope "
    "--property=TimeoutStopSec=15 -- setsid steam steam://open/bigpicture"
  );
}

TEST(DetachedScope, KeepsQuotedArguments) {
  const auto line = scope::wrap("/usr/bin/systemd-run", "u.scope", 20s, "\"/opt/My Game/run.sh\" --flag");
  EXPECT_TRUE(line.ends_with("-- \"/opt/My Game/run.sh\" --flag"));
  EXPECT_NE(line.find("TimeoutStopSec=20"), std::string::npos);
}
