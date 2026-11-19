/**
 * @file tests/unit/test_frame_limit_env.cpp
 * @brief The environment that caps an application at the stream's frame rate.
 */
#include "../tests_common.h"

#include <src/frame_limit_env.h>

using proc::frame_limit::environment;
using proc::frame_limit::mangohud_config;

TEST(FrameLimitEnv, NoMangoHudConfigKeepsTheUsersConfigFile) {
  EXPECT_EQ(mangohud_config("", 120), "read_cfg,fps_limit=120");
}

TEST(FrameLimitEnv, AnExistingConfigIsKeptAndOnlyItsLimitReplaced) {
  EXPECT_EQ(mangohud_config("read_cfg,no_display,fps_limit=60", 120), "read_cfg,no_display,fps_limit=120");
  EXPECT_EQ(mangohud_config("fps,frametime", 90), "fps,frametime,fps_limit=90")
    << "without read_cfg the user chose not to read the file; adding it would change their overlay";
  EXPECT_EQ(mangohud_config("fps_limit=30+60", 60), "fps_limit=60");
}

TEST(FrameLimitEnv, EveryLimiterGetsTheRateAndMangoHudIsNotTurnedOn) {
  const auto env = environment(120, "");
  ASSERT_EQ(env.size(), 3u);
  EXPECT_EQ(env[0], (std::pair<std::string, std::string> {"DXVK_FRAME_RATE", "120"}));
  EXPECT_EQ(env[1], (std::pair<std::string, std::string> {"VKD3D_FRAME_RATE", "120"}));
  EXPECT_EQ(env[2], (std::pair<std::string, std::string> {"MANGOHUD_CONFIG", "read_cfg,fps_limit=120"}));
  for (const auto &[name, value] : env) {
    EXPECT_NE(name, "MANGOHUD");
  }
}

TEST(FrameLimitEnv, NoRateNoLimit) {
  EXPECT_TRUE(environment(0, "").empty());
}
