/**
 * @file tests/unit/platform/test_wayland_capture.cpp
 * @brief End-to-end tests for the Wayland capture protocols against a live compositor.
 *
 * Protocol code cannot be tested against a mock in any way that means much: the
 * thing that breaks is the sequence a real compositor expects, and a mock only
 * ever confirms the sequence we already believe in. So these talk to whatever
 * compositor is on WAYLAND_DISPLAY and skip when there is none, or when the one
 * there does not offer the protocol under test.
 *
 * To run them against a compositor other than the desktop's:
 *
 *     WLR_BACKENDS=headless WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 labwc &
 *     WAYLAND_DISPLAY=wayland-1 ./tests/test_sunshine --gtest_filter='WaylandCapture*'
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  #include <algorithm>
  #include <chrono>
  #include <fcntl.h>
  #include <filesystem>
  #include <string_view>
  #include <unistd.h>
  #include <xf86drm.h>

  #include <src/platform/linux/wayland.h>

using namespace std::chrono_literals;

namespace {
  /**
   * @brief A connected compositor with its interfaces bound and outputs listed.
   *
   * Returns nullptr when there is no compositor to talk to, which is the normal
   * case in CI and the reason every test here starts by checking for one.
   */
  struct session_t {
    wl::display_t display;
    wl::interface_t interface;

    static std::unique_ptr<session_t> open() {
      auto session = std::make_unique<session_t>();
      if (session->display.init()) {
        return nullptr;
      }

      session->interface.listen(session->display.registry());
      session->display.roundtrip();

      if (session->interface.monitors.empty()) {
        return nullptr;
      }

      for (auto &monitor : session->interface.monitors) {
        monitor->listen(session->interface.output_manager);
      }
      session->display.roundtrip();

      return session;
    }

    bool has_icc() const {
      return interface[wl::interface_t::IMAGE_COPY_CAPTURE] &&
             interface[wl::interface_t::IMAGE_CAPTURE_SOURCE] &&
             interface[wl::interface_t::LINUX_DMABUF];
    }

    bool has_screencopy() const {
      return interface[wl::interface_t::WLR_EXPORT_DMABUF] &&
             interface[wl::interface_t::LINUX_DMABUF];
    }

    wl::monitor_t *first_monitor() {
      return interface.monitors[0].get();
    }
  };

  /**
   * @brief Pump the display until the capture leaves WAITING or the deadline passes.
   */
  bool pump(session_t &session, wl::dmabuf_t &dmabuf, bool icc, bool cursor) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (dmabuf.status == wl::dmabuf_t::WAITING) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()
      );
      if (remaining.count() <= 0 || !session.display.dispatch(remaining)) {
        return false;
      }
      if (icc && dmabuf.status == wl::dmabuf_t::WAITING) {
        // Mirrors wlr_t::snapshot: the frame can only be asked for once the
        // session has finished sending its constraints, and "done" arrives
        // here - but only while still waiting, or a finished frame is thrown
        // away and re-armed.
        dmabuf.icc_capture(cursor);
      }
    }

    return true;
  }
}  // namespace

class WaylandCaptureTest: public ::testing::Test {
protected:
  void SetUp() override {
    session = session_t::open();
    if (!session) {
      GTEST_SKIP() << "No Wayland compositor with outputs on WAYLAND_DISPLAY";
    }
  }

  std::unique_ptr<session_t> session;
};

TEST_F(WaylandCaptureTest, ImageCopyCaptureProducesAFrame) {
  if (!session->has_icc()) {
    GTEST_SKIP() << "Compositor does not offer ext-image-copy-capture with linux-dmabuf";
  }

  auto monitor = session->first_monitor();
  wl::dmabuf_t dmabuf;

  ASSERT_TRUE(dmabuf.icc_listen(
    session->interface.image_copy_capture_manager,
    session->interface.image_capture_source_manager,
    session->interface.dmabuf_interface,
    monitor->output,
    false
  )) << "Could not open a capture session on " << monitor->name;

  dmabuf.icc_capture(false);
  ASSERT_TRUE(pump(*session, dmabuf, true, false)) << "Timed out waiting for a frame";
  ASSERT_EQ(dmabuf.status, wl::dmabuf_t::READY) << "Capture ended in status " << (int) dmabuf.status;

  // The buffer the compositor filled is the output's size, in a real format,
  // backed by a descriptor the encoder can import. A capture that "succeeds"
  // without those is the black-screen failure this backend exists to avoid.
  const auto &sd = dmabuf.current_frame->sd;
  EXPECT_EQ(sd.width, (std::uint32_t) monitor->viewport.width);
  EXPECT_EQ(sd.height, (std::uint32_t) monitor->viewport.height);
  EXPECT_NE(sd.fourcc, 0u);
  EXPECT_GE(sd.fds[0], 0);
  EXPECT_GT(sd.pitches[0], 0u);
}

TEST_F(WaylandCaptureTest, ImageCopyCaptureRepeatsOnOneSession) {
  if (!session->has_icc()) {
    GTEST_SKIP() << "Compositor does not offer ext-image-copy-capture with linux-dmabuf";
  }

  auto monitor = session->first_monitor();
  wl::dmabuf_t dmabuf;

  ASSERT_TRUE(dmabuf.icc_listen(
    session->interface.image_copy_capture_manager,
    session->interface.image_capture_source_manager,
    session->interface.dmabuf_interface,
    monitor->output,
    false
  ));

  // The session is negotiated once and reused; a second frame that needs a
  // second session would mean the streaming path renegotiates every frame.
  for (int frame = 0; frame < 3; ++frame) {
    dmabuf.icc_capture(false);

    if (!pump(*session, dmabuf, true, false)) {
      // A frame completes when the output is next presented, so a compositor
      // with nothing moving on it legitimately never finishes this one. That is
      // not a defect - the streaming path reads it as capture_e::timeout and
      // re-sends the previous frame - but it does mean this test needs a
      // compositor that repaints, and cannot assert anything without one.
      ASSERT_TRUE(dmabuf.icc_ready()) << "Session died rather than stalling on frame " << frame;
      GTEST_SKIP() << "Compositor is idle: ext-image-copy-capture completes a frame only on the "
                      "next presentation. Run a client that draws (weston-simple-egl) to exercise "
                      "repeated capture.";
    }

    ASSERT_EQ(dmabuf.status, wl::dmabuf_t::READY) << "Frame " << frame << " failed";
    ASSERT_TRUE(dmabuf.icc_ready()) << "Session was torn down after frame " << frame;
  }
}

TEST_F(WaylandCaptureTest, ScreencopyStillProducesAFrame) {
  if (!session->has_screencopy()) {
    GTEST_SKIP() << "Compositor does not offer wlr-screencopy with linux-dmabuf";
  }

  auto monitor = session->first_monitor();
  wl::dmabuf_t dmabuf;

  dmabuf.listen(
    session->interface.screencopy_manager,
    session->interface.dmabuf_interface,
    monitor->output,
    false
  );

  ASSERT_TRUE(pump(*session, dmabuf, false, false)) << "Timed out waiting for a frame";
  ASSERT_EQ(dmabuf.status, wl::dmabuf_t::READY);

  const auto &sd = dmabuf.current_frame->sd;
  EXPECT_EQ(sd.width, (std::uint32_t) monitor->viewport.width);
  EXPECT_EQ(sd.height, (std::uint32_t) monitor->viewport.height);
  EXPECT_NE(sd.fourcc, 0u);
  EXPECT_GE(sd.fds[0], 0);
}

/**
 * @brief Render nodes on this machine, by driver name.
 *
 * Used to find a node that cannot possibly allocate - a Hermes-KMS capture node
 * has no Mesa driver behind it - without hardcoding a number that follows module
 * load order.
 */
static std::vector<std::string> render_nodes_for_driver(bool hermes_kms) {
  std::vector<std::string> nodes;
  std::error_code ec;
  std::filesystem::directory_iterator dir {"/dev/dri", ec};
  if (ec) {
    return nodes;
  }
  for (const auto &entry : dir) {
    if (!entry.path().filename().string().starts_with("renderD")) {
      continue;
    }
    const int fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr version = drmGetVersion(fd);
    const bool matches = version && version->name &&
                         ((std::string_view {version->name} == "hermes-kms") == hermes_kms);
    if (version) {
      drmFreeVersion(version);
    }
    ::close(fd);
    if (matches) {
      nodes.emplace_back(entry.path().string());
    }
  }
  return nodes;
}

TEST(WaylandRenderNodeProbe, RejectsWhatCannotBeOpened) {
  EXPECT_FALSE(wl::render_node_can_allocate(nullptr));
  EXPECT_FALSE(wl::render_node_can_allocate(""));
  EXPECT_FALSE(wl::render_node_can_allocate("/dev/dri/renderD-does-not-exist"));
}

TEST(WaylandRenderNodeProbe, RejectsANodeThatOpensButCannotAllocate) {
  // This is the whole reason the probe exists. A Hermes-KMS render node opens
  // like any other and has no Mesa driver behind it, so Mesa falls back to
  // kms_swrast and every allocation fails - the same shape as a
  // proprietary-NVIDIA node on a hybrid machine, which is what put a capture on
  // a GPU the compositor never rendered on.
  const auto nodes = render_nodes_for_driver(true);
  if (nodes.empty()) {
    GTEST_SKIP() << "No Hermes-KMS render node on this machine";
  }
  for (const auto &node : nodes) {
    EXPECT_FALSE(wl::render_node_can_allocate(node.c_str()))
      << node << " is a capture-only node and must not be offered as a render device";
  }
}

TEST(WaylandRenderNodeProbe, AcceptsAGpuThatCanAllocate) {
  const auto nodes = render_nodes_for_driver(false);
  if (nodes.empty()) {
    GTEST_SKIP() << "No non-Hermes-KMS render node on this machine";
  }
  const bool any = std::any_of(nodes.begin(), nodes.end(), [](const auto &node) {
    return wl::render_node_can_allocate(node.c_str());
  });
  if (!any) {
    GTEST_SKIP() << "No render node here can allocate; nothing to assert about a working GPU";
  }
  SUCCEED();
}

TEST_F(WaylandCaptureTest, ProbeAgreesWithWhatWasBound) {
  // probe_protocols() is what the readiness report answers from, and it opens
  // its own connection rather than reading the one above. The two disagreeing
  // would mean the web UI describes a session Hermes does not capture.
  const auto probed = wl::probe_protocols();
  ASSERT_TRUE(probed.connected);

  EXPECT_EQ(probed.screencopy, session->interface[wl::interface_t::WLR_EXPORT_DMABUF]);
  EXPECT_EQ(probed.linux_dmabuf, session->interface[wl::interface_t::LINUX_DMABUF]);
  EXPECT_EQ(probed.image_copy_capture, session->interface[wl::interface_t::IMAGE_COPY_CAPTURE]);
  EXPECT_EQ(probed.output_capture_source, session->interface[wl::interface_t::IMAGE_CAPTURE_SOURCE]);
  EXPECT_EQ(probed.xdg_output, session->interface[wl::interface_t::XDG_OUTPUT]);
}

#endif  // SUNSHINE_BUILD_WAYLAND
