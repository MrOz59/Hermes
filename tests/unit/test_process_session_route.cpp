/**
 * @file tests/unit/test_process_session_route.cpp
 * @brief Test host-vs-independent session routing.
 */
#include "../tests_common.h"

#include <src/process.h>

TEST(ProcessSessionRoute, VirtualDisplayRequestIsExplicitPolicy) {
  proc::ctx_t app {};
  rtsp_stream::launch_session_t launch {};

  EXPECT_FALSE(proc::launch_requests_virtual_display(app, launch, false));

  app.virtual_display = true;
  EXPECT_TRUE(proc::launch_requests_virtual_display(app, launch, false));

  app.virtual_display = false;
  launch.virtual_display = true;
  EXPECT_TRUE(proc::launch_requests_virtual_display(app, launch, false));

  launch.virtual_display = false;
  EXPECT_TRUE(proc::launch_requests_virtual_display(app, launch, true));
}

TEST(ProcessSessionRoute, DisabledCapabilityAlwaysUsesHost) {
  proc::ctx_t app {};
  app.session_type = "desktop";

  EXPECT_EQ(
    proc::resolve_session_route(app, false, true),
    proc::session_route_e::shared
  );
}

TEST(ProcessSessionRoute, SharedAlwaysUsesHost) {
  proc::ctx_t app {};
  app.session_type = "shared";

  EXPECT_EQ(
    proc::resolve_session_route(app, true, true),
    proc::session_route_e::shared
  );
}

TEST(ProcessSessionRoute, AutoWithoutVirtualDisplayRequestUsesHost) {
  proc::ctx_t app {};
  app.session_type = "auto";

  EXPECT_EQ(
    proc::resolve_session_route(app, true, false),
    proc::session_route_e::shared
  );
}

TEST(ProcessSessionRoute, ExplicitDesktopForcesIsolatedDesktop) {
  proc::ctx_t app {};
  app.session_type = "desktop";

  EXPECT_EQ(
    proc::resolve_session_route(app, true, false),
    proc::session_route_e::isolated_desktop
  );
}

TEST(ProcessSessionRoute, ExplicitDesktopWithVirtualDisplayIsIsolated) {
  proc::ctx_t app {};
  app.session_type = "desktop";

  EXPECT_EQ(
    proc::resolve_session_route(app, true, true),
    proc::session_route_e::isolated_desktop
  );
}

TEST(ProcessSessionRoute, ExplicitApplicationWithVirtualDisplayIsIsolated) {
  proc::ctx_t app {};
  app.session_type = "application";
  app.cmd = "game";

  EXPECT_EQ(
    proc::resolve_session_route(app, true, true),
    proc::session_route_e::isolated_application
  );
}

TEST(ProcessSessionRoute, AutoNonDetachedLayoutsUseHost) {
  for (const char *layout : {"auto", "extend", "mirror", "exclusive"}) {
    proc::ctx_t app {};
    app.session_type = "auto";
    app.virtual_display_layout = layout;

    EXPECT_EQ(
      proc::resolve_session_route(app, true, true),
      proc::session_route_e::shared
    ) << layout;
  }
}

TEST(ProcessSessionRoute, AutoVirtualDisplaySelectsProfileFromCommand) {
  proc::ctx_t desktop {};
  desktop.session_type = "auto";
  desktop.virtual_display_layout = "detached";

  EXPECT_EQ(
    proc::resolve_session_route(desktop, true, true),
    proc::session_route_e::isolated_desktop
  );

  proc::ctx_t application {};
  application.session_type = "auto";
  application.virtual_display_layout = "detached";
  application.cmd = "game";

  EXPECT_EQ(
    proc::resolve_session_route(application, true, true),
    proc::session_route_e::isolated_application
  );
}
