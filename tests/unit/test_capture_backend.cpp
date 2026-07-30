/**
 * @file tests/unit/test_capture_backend.cpp
 * @brief Headless tests for the injectable Hermes capture-backend boundary.
 */

#include "../tests_common.h"

#include <memory>
#include <src/capture_backend.h>
#include <src/video.h>
#include <string>
#include <utility>
#include <vector>

namespace {

  class fake_display_t final:
      public platf::display_t {
  public:
    platf::capture_e capture(
      const push_captured_image_cb_t &,
      const pull_free_image_cb_t &,
      bool *
    ) override {
      ++capture_calls;
      return capture_result;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      ++alloc_calls;
      return nullptr;
    }

    int dummy_img(platf::img_t *) override {
      ++dummy_calls;
      return dummy_result;
    }

    platf::capture_e capture_result = platf::capture_e::reinit;
    int dummy_result = 23;
    int capture_calls = 0;
    int alloc_calls = 0;
    int dummy_calls = 0;
  };

  class fake_capture_backend_t final:
      public video::capture_backend::ICaptureBackend {
  public:
    std::vector<std::string> enumerate_displays(
      platf::mem_type_e memory_type
    ) override {
      ++enumerate_calls;
      last_enumeration_memory_type = memory_type;
      return displays;
    }

    std::shared_ptr<platf::display_t> create_display(
      platf::mem_type_e memory_type,
      const std::string &display_name,
      const video::config_t &config
    ) override {
      ++create_calls;
      last_creation_memory_type = memory_type;
      last_display_name = display_name;
      last_config = &config;
      return display;
    }

    std::vector<std::string> displays;
    std::shared_ptr<platf::display_t> display;
    const video::config_t *last_config = nullptr;
    platf::mem_type_e last_enumeration_memory_type =
      platf::mem_type_e::unknown;
    platf::mem_type_e last_creation_memory_type =
      platf::mem_type_e::unknown;
    std::string last_display_name;
    int enumerate_calls = 0;
    int create_calls = 0;
  };

}  // namespace

TEST(CaptureBackendTest, InterfaceForwardsSelectionAndReturnsCaptureSession) {
  auto expected_display = std::make_shared<fake_display_t>();
  fake_capture_backend_t fake;
  fake.displays = {"card0-HDMI-A-1", "card1-DP-2"};
  fake.display = expected_display;
  video::capture_backend::ICaptureBackend &backend = fake;

  video::config_t config {};
  config.width = 2560;
  config.height = 1440;
  config.display_name = "card1-DP-2";

  const auto displays =
    backend.enumerate_displays(platf::mem_type_e::vaapi);
  const auto display = backend.create_display(
    platf::mem_type_e::vaapi,
    config.display_name,
    config
  );

  EXPECT_EQ(displays, fake.displays);
  EXPECT_EQ(display, expected_display);
  EXPECT_EQ(fake.enumerate_calls, 1);
  EXPECT_EQ(fake.create_calls, 1);
  EXPECT_EQ(
    fake.last_enumeration_memory_type,
    platf::mem_type_e::vaapi
  );
  EXPECT_EQ(
    fake.last_creation_memory_type,
    platf::mem_type_e::vaapi
  );
  EXPECT_EQ(fake.last_display_name, config.display_name);
  EXPECT_EQ(fake.last_config, &config);

  EXPECT_EQ(
    display->capture({}, {}, nullptr),
    platf::capture_e::reinit
  );
  EXPECT_EQ(expected_display->capture_calls, 1);
}

TEST(CaptureBackendTest, FakeCanModelUnavailablePlatformWithoutInitialization) {
  fake_capture_backend_t fake;
  video::capture_backend::ICaptureBackend &backend = fake;
  const video::config_t config {};

  EXPECT_TRUE(
    backend.enumerate_displays(platf::mem_type_e::system).empty()
  );
  EXPECT_EQ(
    backend.create_display(
      platf::mem_type_e::system,
      "missing-display",
      config
    ),
    nullptr
  );
  EXPECT_EQ(fake.enumerate_calls, 1);
  EXPECT_EQ(fake.create_calls, 1);
  EXPECT_EQ(fake.last_display_name, "missing-display");
  EXPECT_EQ(fake.last_config, &config);
}
