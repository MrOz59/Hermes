#include "../../tests_common.h"
#include "src/platform/linux/gamescope.h"

#include <map>
#include <string>

TEST(GamescopeHdr, SessionTransitionsReplaceInheritedSdrOverrides) {
  std::map<std::string, std::string> env {{"DXVK_HDR", "0"}, {"PROTON_ENABLE_HDR", "0"}};
  for (const bool hdr : {true, false, true}) {
    platf::gamescope::set_hdr_environment(env, hdr);
    EXPECT_EQ(env["DXVK_HDR"], hdr ? "1" : "0");
    EXPECT_EQ(env["PROTON_ENABLE_HDR"], hdr ? "1" : "0");
    for (const auto *name : {"HERMES_CLIENT_HDR", "APOLLO_CLIENT_HDR", "SUNSHINE_CLIENT_HDR"}) {
      EXPECT_EQ(env[name], hdr ? "true" : "false");
    }
  }
}

TEST(GamescopeHdr, OnlyColorManagedBackendsAcceptHdr) {
  for (const auto *backend : {"", "auto", "wayland", "drm"}) {
    EXPECT_TRUE(platf::gamescope::supports_hdr_backend(backend));
  }
  EXPECT_FALSE(platf::gamescope::supports_hdr_backend("sdl"));
  EXPECT_FALSE(platf::gamescope::supports_hdr_backend("headless"));
  EXPECT_STREQ(platf::gamescope::hdr_arguments(true), " --hdr-enabled");
  EXPECT_STREQ(platf::gamescope::hdr_arguments(false), "");
}
