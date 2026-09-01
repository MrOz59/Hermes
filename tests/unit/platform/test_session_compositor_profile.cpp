/**
 * @file tests/unit/platform/test_session_compositor_profile.cpp
 * @brief Test the session compositor profiles Hermes ships.
 *
 * A profile is the whole of what differs between compositors - "a different
 * compositor is a file, not a patch" - which means a typo in one of these files
 * is a session that does not start, on a path that only exists on a machine with
 * the Hermes-KMS driver, a private seat and an account broker. The expansion
 * itself needs none of that, so it is checked here instead.
 */
#include "../../tests_common.h"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace proc {
  bool expand_compositor_profile_for_test(
    const std::string &path,
    const std::string &name,
    const std::vector<std::pair<std::string, std::string>> &values,
    std::string &command,
    std::vector<std::pair<std::string, std::string>> &env,
    std::string &config_template,
    std::string &config_target,
    bool &discover_socket,
    std::string &required_binary
  );
}

namespace {
  const std::filesystem::path profile_dir {
    std::filesystem::path {SUNSHINE_SOURCE_DIR} / "src_assets" / "linux" / "misc" / "session-compositors"
  };

  // The values a session actually hands a profile: a Hermes-KMS card on its own
  // seat, a real GPU to render with, and the mode the client asked for.
  const std::vector<std::pair<std::string, std::string>> session_values {
    {"card", "card4"},
    {"card_path", "/dev/dri/card4"},
    {"seat", "hermes-kms-1"},
    {"socket", "hermes-session-1"},
    {"log", "/run/user/1000/hermes/session-1/compositor.log"},
    {"runtime_dir", "/run/user/1000/hermes/session-1"},
    {"render_node", "/dev/dri/renderD128"},
    {"width", "1280"},
    {"height", "720"},
    {"refresh", "60"},
    {"connector", "Virtual-2"},
    {"config", "/run/user/1000/hermes/session-1/labwc/autostart"},
    {"config_dir", "/run/user/1000/hermes/session-1/labwc"},
  };

  struct expansion_t {
    std::string command;
    std::vector<std::pair<std::string, std::string>> env;
    std::string config_template;
    std::string config_target;
    bool discover_socket {false};
    std::string required_binary;

    std::string env_value(const std::string &key) const {
      for (const auto &[k, v] : env) {
        if (k == key) {
          return v;
        }
      }
      return {};
    }
  };

  testing::AssertionResult expand(const std::string &name, expansion_t &out) {
    const auto path = profile_dir / (name + ".conf");
    if (!std::filesystem::is_regular_file(path)) {
      return testing::AssertionFailure() << "No profile shipped at " << path;
    }
    if (!proc::expand_compositor_profile_for_test(
          path.string(),
          name,
          session_values,
          out.command,
          out.env,
          out.config_template,
          out.config_target,
          out.discover_socket,
          out.required_binary
        )) {
      return testing::AssertionFailure() << "Profile " << path << " failed to load";
    }
    return testing::AssertionSuccess();
  }
}  // namespace

TEST(SessionCompositorProfile, LabwcRendersOnARealGpuAndScansOutOnItsOwnCard) {
  expansion_t labwc;
  ASSERT_TRUE(expand("labwc", labwc));

  // The point of the profile: the display device is the session's own card, the
  // rendering device is a real GPU. Weston cannot express that split, which is
  // why a session under it composites in software.
  EXPECT_EQ(labwc.env_value("WLR_DRM_DEVICES"), "/dev/dri/card4");
  EXPECT_EQ(labwc.env_value("WLR_RENDER_DRM_DEVICE"), "/dev/dri/renderD128");
  EXPECT_EQ(labwc.env_value("WLR_BACKENDS"), "drm");

  // wlroots opens DRM devices through libseat, whose logind backend resolves the
  // calling process's own logind session - which a transient unit on a private
  // seat does not have. noop opens the path directly, which the card's
  // access_uid has already permitted.
  EXPECT_EQ(labwc.env_value("LIBSEAT_BACKEND"), "noop");

  EXPECT_EQ(labwc.required_binary, "labwc");
  EXPECT_TRUE(labwc.discover_socket) << "labwc names its own socket";
}

TEST(SessionCompositorProfile, LabwcIsGivenTheDirectoryItsConfigWasWrittenInto) {
  expansion_t labwc;
  ASSERT_TRUE(expand("labwc", labwc));

  // labwc reads a directory, not a file, so config-target names a file inside
  // one and {config_dir} is what reaches the command line. Getting this wrong
  // means labwc silently runs with default configuration and the wrong mode.
  EXPECT_EQ(labwc.config_target, "/run/user/1000/hermes/session-1/labwc/autostart");
  EXPECT_EQ(labwc.command, "labwc -C /run/user/1000/hermes/session-1/labwc");
  EXPECT_EQ(
    std::filesystem::path {labwc.config_target}.parent_path().string(),
    "/run/user/1000/hermes/session-1/labwc"
  ) << "the directory handed to labwc must be the one the config was written into";
}

TEST(SessionCompositorProfile, EveryProfileTemplateItNamesIsShipped) {
  for (const auto *name : {"labwc", "weston"}) {
    expansion_t profile;
    ASSERT_TRUE(expand(name, profile)) << name;
    if (profile.config_template.empty()) {
      continue;
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(profile.config_template))
      << name << " names a config template that is not shipped: " << profile.config_template;
  }
}

TEST(SessionCompositorProfile, LabwcAutostartPinsTheModeTheClientAsked) {
  expansion_t labwc;
  ASSERT_TRUE(expand("labwc", labwc));
  ASSERT_FALSE(labwc.config_template.empty());

  // The template is expanded with the same values as the command line, so the
  // mode it names has to be the client's - a placeholder that never reaches it
  // leaves the session on whatever mode wlroots preferred, which is the bug this
  // file exists to avoid.
  std::ifstream in {labwc.config_template};
  std::stringstream buffer;
  buffer << in.rdbuf();
  const auto text = buffer.str();
  EXPECT_NE(text.find("{width}x{height}@{refresh}"), std::string::npos)
    << "the autostart template no longer names the requested mode";
  EXPECT_NE(text.find("{connector}"), std::string::npos)
    << "the autostart template no longer names the output";
  EXPECT_NE(text.find("wlr-randr"), std::string::npos);
}

TEST(SessionCompositorProfile, WestonStillTakesTheSocketItIsGiven) {
  // The weston profile is the one every existing deployment runs; labwc must not
  // have changed how it is read.
  expansion_t weston;
  ASSERT_TRUE(expand("weston", weston));

  EXPECT_FALSE(weston.discover_socket) << "Hermes names weston's socket";
  EXPECT_EQ(weston.required_binary, "weston");
  EXPECT_NE(weston.command.find("--socket=hermes-session-1"), std::string::npos);
  EXPECT_NE(weston.command.find("--drm-device=card4"), std::string::npos);
  EXPECT_NE(weston.command.find("--seat=hermes-kms-1"), std::string::npos);
  EXPECT_EQ(weston.config_target, "/run/user/1000/hermes/session-1/weston.ini");
}
